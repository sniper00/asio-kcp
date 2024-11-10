#include <string>
#include <memory>
#include <array>
#include <unordered_set>
#include <unordered_map>
#include <cstdarg>
#include <queue>
#include <ctime>
#include <chrono>
#include <asio.hpp>
#include <asio/as_tuple.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <ikcp.h>
#include "error.hpp"

namespace moon
{
    using asio::as_tuple_t;
    using asio::awaitable;
    using asio::co_spawn;
    using asio::use_awaitable_t;
    using namespace asio::experimental::awaitable_operators;


    using std::chrono::steady_clock;
    using namespace std::literals::chrono_literals;
    using namespace std::string_view_literals;

    namespace kcp
    {
        constexpr time_t timeout_duration = 30 * 1000;//millseconds
        constexpr time_t update_interval = 5;//millseconds

        constexpr uint8_t opcode_handshark = 1;
        constexpr uint8_t opcode_handshark_response = 2;
        constexpr uint8_t opcode_keepalive = 3;
        constexpr uint8_t opcode_data = 4;
        constexpr uint8_t opcode_disconnect = 5;
        constexpr uint8_t opcode_max = 6;
        constexpr size_t idle_send_packet_count = 128;

        using asio::ip::udp;

        static std::tm* localtime(std::time_t* t, std::tm* result)
        {
#ifdef _MSC_VER
            localtime_s(result, t);
#else
            localtime_r(t, result);
#endif
            return result;
        }

        inline void console_log(const char* format, ...)
        {
            auto now = time(nullptr);
            std::tm m;
            localtime(&now, &m);
            char tmbuffer[80];
            strftime(tmbuffer, 80, "%Y-%m-%d, %H:%M:%S", &m);

            va_list args;
            va_start(args, format);
            char buffer[4 * 1024];
            vsprintf(buffer, format, args);
            printf("[%s] %s\n", tmbuffer, buffer);
            va_end(args);
            fflush(stdout);
        }

        inline std::pair<std::string, unsigned short> parse_host_port(const std::string& host_port, unsigned short default_port) {
            std::string host, port;
            host.reserve(host_port.size());
            bool parse_port = false;
            int square_count = 0; // To parse IPv6 addresses
            for (auto chr : host_port) {
                if (chr == '[')
                    ++square_count;
                else if (chr == ']')
                    --square_count;
                else if (square_count == 0 && chr == ':')
                    parse_port = true;
                else if (!parse_port)
                    host += chr;
                else
                    port += chr;
            }

            if (port.empty())
                return { std::move(host), default_port };
            else {
                try {
                    return { std::move(host), static_cast<unsigned short>(std::stoul(port)) };
                }
                catch (...) {
                    return { std::move(host), default_port };
                }
            }
        }

        template<typename EndPoint>
        static std::string address(const EndPoint& endpoint)
        {
            std::string address;
            asio::error_code code;

            address.append(endpoint.address().to_string());
            address.append(":");
            address.append(std::to_string(endpoint.port()));
            return address;
        }

        inline time_t clock()
        {
            using time_point = std::chrono::time_point<std::chrono::steady_clock>;
            static const time_point start_time_point = std::chrono::steady_clock::now();
            auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_point);
            return diff.count();
        }

        class static_buffer
        {
            size_t size_ = 0;
            std::array<char, 2048> data_{};
        public:
            char* data()
            {
                return data_.data();
            }

            const char* data() const
            {
                return data_.data();
            }

            size_t max_size() const
            {
                return data_.max_size();
            }

            void set_size(size_t n)
            {
                size_ = n;
            }

            size_t size() const
            {
                return size_;
            }

            asio::mutable_buffer mutable_buffer()
            {
                return asio::buffer(data_.data(), size_);
            }

            asio::const_buffer const_buffer() const
            {
                return asio::buffer(data_.data(), size_);
            }

            std::string to_string() const
            {
                return std::string{ data_.data(), size_ };
            }

            void write(uint8_t opcode, const char* data, size_t size)
            {
                assert(size+1 < data_.size());
                data_[0] = opcode;
                size_ = 1 + size;
                memcpy(data_.data() + 1, data, size);
            }
        };

        class static_buffer_pool
        {
            struct container
            {
                ~container()
                {
                    for (auto p : pool)
                    {
                        delete p;
                    }
                }
                std::vector<static_buffer*> pool;
            };

            struct static_buffer_deleter {
                void operator()(static_buffer* p)
                {
                    if (container_.pool.size() < 1024)
                    {
                        container_.pool.push_back(p);
                    }
                    else
                    {
                        delete p;
                    }
                }
            };
        public:
            using static_buffer_ptr_t = std::unique_ptr<static_buffer, static_buffer_deleter>;

            static static_buffer_ptr_t make()
            {
                static_buffer* buffer;
                if (!container_.pool.empty())
                {
                    buffer = container_.pool.back();
                    container_.pool.pop_back();
                    buffer->set_size(0);
                }
                else
                {
                    buffer = new static_buffer{};
                }
                return static_buffer_ptr_t{ buffer };
            }
        private:
            inline static thread_local container container_{};
        };

        class operation
        {
        public:
            bool complete(void* owner, const std::error_code& ec,std::size_t bytes_transferred)
            {
                return func_(owner, this, ec, bytes_transferred);
            }
        protected:
            using func_type = bool(*)(void*, operation*, const std::error_code&, std::size_t);
            operation(func_type func):func_(func){}
            ~operation(){}
        private:
            func_type func_;
        };

        //--------------------------------connection--------------------------------------

        class connection: public std::enable_shared_from_this<connection>
        {
            friend class acceptor;

            struct kcp_obj_deleter{void operator()(ikcpcb* p){ikcp_release(p);}};

            using kcp_context_ptr = std::unique_ptr<ikcpcb, kcp_obj_deleter>;

            using static_buffer_ptr_t = static_buffer_pool::static_buffer_ptr_t;

        private:
            enum class state
            {
                idle = 0,
                opened = 1,
                closed = 2,
            };

            bool isserver_ = true;
            state state_ = state::idle;
            uint32_t conv_ = 0;
            time_t next_tick_ = 0;
            time_t now_tick_ = 0;
            udp::socket* sock_;
            udp::endpoint endpoint_;
            std::unique_ptr<ikcpcb, kcp_obj_deleter> obj_;
            std::unique_ptr<asio::steady_timer> timer_;
            std::shared_ptr<operation> read_op_;
            std::shared_ptr<operation> write_op_;

            template <typename Handler, typename MutableBuffer>
            class read_some_op :public operation
            {
            public:
                read_some_op(Handler&& handler, const MutableBuffer& buffer)
                    :operation(&read_some_op::do_complete)
                    , handler_(std::move(handler))
                    , buffer_(buffer)
                {

                }

                static bool do_complete(void* user, operation* base, const asio::error_code& ec, std::size_t)
                {
                    read_some_op* op(static_cast<read_some_op*>(base));
                    if (ec)
                    {
                        op->handler_(ec, 0);
                        return true;
                    }
                    else
                    {
                        connection* c = static_cast<connection*>(user);
                        int n = ikcp_recv(c->obj_.get(), reinterpret_cast<char*>(op->buffer_.data()), static_cast<int>(op->buffer_.size()));
                        if (n > 0)
                        {
                            op->handler_(ec, n);
                            return true;
                        }
                        return false;
                    }
                }

                size_t buffer_size() const
                {
                    return buffer_.size();
                }
            private:
                Handler handler_;
                const MutableBuffer& buffer_;
            };

            template <typename Handler, typename StreamBuffer>
            class read_op :public operation
            {
            public:
                read_op(Handler&& handler, StreamBuffer& buffer, size_t need)
                    :operation(&read_op::do_complete)
                    , handler_(std::move(handler))
                    , buffer_(buffer)
                    , need_(need)
                {
                }

                static bool do_complete(void* user, operation* base, const asio::error_code& ec, std::size_t)
                {
                    read_op* op(static_cast<read_op*>(base));
                    if (ec)
                    {
                        op->handler_(ec, 0);
                        return true;
                    }
                    else
                    {
                        connection* c = static_cast<connection*>(user);
                        while (true)
                        {
                            auto buffer = op->buffer_.prepare(2048);
                            int n = ikcp_recv(c->obj_.get(), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()));
                            if (n > 0)
                            {
                                op->buffer_.commit(n);
                            }
                            else
                            {
                                break;
                            }
                        }

                        if (op->buffer_.size() >= op->need_)
                        {
                            op->handler_(ec, op->need_);
                            return true;
                        }
                        return false;
                    }
                }

                size_t buffer_size() const
                {
                    return buffer_.size();
                }
            private:
                Handler handler_;
                StreamBuffer& buffer_;
                size_t need_;
            };

            template<bool some>
            class initiate_async_read
            {
            public:
                typedef  asio::any_io_executor executor_type;

                explicit initiate_async_read(connection* self)
                    : self_(self)
                {
                }

                executor_type get_executor() const
                {
                    return self_->get_executor();
                }

                template<typename ReadHandler, typename Buffer>
                void operator()(ReadHandler&& handler, Buffer&& buffer, size_t need) const
                {
                    if (nullptr != self_->read_op_ || self_->closed())
                    {
                        auto executor = asio::get_associated_executor(
                            handler, self_->get_executor());

                        asio::post(
                            asio::bind_executor(executor,
                                std::bind(std::forward<decltype(handler)>(
                                    handler), self_->closed()? asio::error::operation_aborted : asio::error::in_progress, 0)));
                    }
                    else
                    {
                        if constexpr(some)
                        {
                            using op_t = read_some_op<std::decay_t<ReadHandler>, std::decay_t<Buffer>>;
                            self_->read_op_ = std::make_unique<op_t>(std::forward<ReadHandler>(handler), buffer);
                            asio::post(get_executor(), [self_ = self_]()
                                       { self_->check_read_op(); });
                        }
                        else
                        {
                            using op_t = read_op<std::decay_t<ReadHandler>, std::decay_t<Buffer>>;
                            self_->read_op_ = std::make_unique<op_t>(std::forward<ReadHandler>(handler), buffer, need);
                            asio::post(get_executor(), [self_ = self_]()
                                       { self_->check_read_op(); });
                        }
                    }
                }

            private:
                connection* self_;
            };


            template <typename Handler>
            class write_op :public operation
            {
            public:
                write_op(Handler&& handler)
                    :operation(&write_op::do_complete)
                    , handler_(std::move(handler))
                {
                }

                static bool do_complete(void* user, operation* base, const asio::error_code& ec, std::size_t)
                {
                    write_op* op(static_cast<write_op*>(base));
                    if (ec)
                    {
                        op->handler_(ec, 0);
                        return true;
                    }
                    else
                    {
                        connection* c = static_cast<connection*>(user);
                        if (ikcp_waitsnd(c->obj_.get()) <= idle_send_packet_count)
                        {
                            op->handler_(ec, 0);
                            return true;
                        }
                        return false;
                    }
                }
            private:
                Handler handler_;
            };

            class initiate_async_write
            {
            public:
                typedef  asio::any_io_executor executor_type;

                explicit initiate_async_write(connection* self)
                    : self_(self)
                {
                }

                executor_type get_executor() const
                {
                    return self_->get_executor();
                }

                template<typename WriteHandler, typename Buffer>
                void operator()(WriteHandler&& handler, Buffer&& buffer) const
                {
                    if (nullptr != self_->write_op_ || self_->closed())
                    {
                        auto executor = asio::get_associated_executor(
                            handler, self_->get_executor());

                        asio::post(
                            asio::bind_executor(executor,
                                std::bind(std::forward<decltype(handler)>(
                                    handler), self_->closed() ? asio::error::operation_aborted : asio::error::in_progress, 0)));
                    }
                    else
                    {
                        self_->next_tick_ = 0;
                        if (ikcp_send(self_->obj_.get(), (const char*)buffer.data(), static_cast<int>(buffer.size())) < 0)
                        {
                            auto executor = asio::get_associated_executor(
                                handler, self_->get_executor());
                            asio::post(
                                asio::bind_executor(executor,
                                    std::bind(std::forward<decltype(handler)>(
                                        handler), asio::error::message_size, 0)));
                            return;
                        }

                        if (ikcp_waitsnd(self_->obj_.get()) <= idle_send_packet_count)
                        {
                            auto executor = asio::get_associated_executor(
                                handler, self_->get_executor());
                            asio::post(
                                asio::bind_executor(executor,
                                    std::bind(std::forward<decltype(handler)>(
                                        handler), asio::error_code{}, 0)));
                        }
                        else
                        {
                            using op_t = write_op<std::decay_t<WriteHandler>>;
                            self_->write_op_ = std::make_unique<op_t>(std::forward<WriteHandler>(handler));
                        }
                    }
                }

            private:
                connection* self_;
            };

            void init_kcp_context()
            {
                obj_ = std::unique_ptr<ikcpcb, kcp_obj_deleter>{ ikcp_create(conv_, this) };
                ikcp_wndsize(obj_.get(), 256, 256);
                ikcp_nodelay(obj_.get(), 1, 10, 2, 1);
                ikcp_setmtu(obj_.get(), 1200);
                obj_->rx_minrto = 10;
                obj_->stream = 1;
                ikcp_setoutput(obj_.get(), [](const char* buf, int len, ikcpcb*, void* user) {
                    connection* conn = (connection*)user;
                    conn->raw_send(buf, len, opcode_data);
                    return 0;
                    });
            }

            void check_read_op(asio::error_code ec = asio::error_code{})
            {
                if (nullptr != read_op_ && nullptr!=obj_)
                {
                    std::shared_ptr<operation> op = std::move(read_op_);
                    if (!op->complete(this, ec, 0))
                    {
                        read_op_ = std::move(op);
                    }
                }
                check_write_op(ec);
            }

            void check_write_op(asio::error_code ec = asio::error_code{})
            {
                if (nullptr != write_op_)
                {
                    std::shared_ptr<operation> op = std::move(write_op_);
                    if (!op->complete(this, ec, 0))
                    {
                        write_op_ = std::move(op);
                    }
                }
            }

            void on_receive(const char* data, size_t size, time_t t)
            {
                if (size < 24)//kcp header
                    return;
                if(state_ == state::idle)
                    state_ = state::opened;

                now_tick_ = t;

                uint8_t opcode = data[0];
                data = data + 1;
                size = size - 1;

                //printf("on_receive opcode %d\n", int(opcode));

                switch (opcode)
                {
                case opcode_data:
                {
                    ikcp_input(obj_.get(), data, (long)size);
                    next_tick_ = 0;
                    check_read_op();
                    return;
                }
                case opcode_keepalive:
                {
                    return;
                }
                case opcode_disconnect:
                {
                    if (nullptr != obj_)
                        close(asio::error::make_error_code(asio::error::eof));
                    return;
                }
                default:
                    break;
                }
            }

            void start_timer()
            {
                if (nullptr == timer_)
                    timer_ = std::make_unique<asio::steady_timer>(get_executor());
                timer_->expires_after(std::chrono::milliseconds(update_interval));
                timer_->async_wait([this, self = shared_from_this()](const asio::error_code& e) {
                    if (e)
                    {
                        return;
                    }
                    time_t now = clock();
                    time_t t = update(now);
                    if ((now - t) > timeout_duration)
                    {
                        close(asio::error::timed_out);
                        return;
                    }
                    start_timer();
                });
            }

            time_t update(time_t now)
            {
                if (obj_ != nullptr && next_tick_ <= now)
                {
                    ikcp_update(obj_.get(), (IUINT32)now);
                    next_tick_ = ikcp_check(obj_.get(), (IUINT32)now);
                    if(next_tick_- now < update_interval)
                        ikcp_flush(obj_.get());
                }
                return now_tick_;
            }

            void do_receive(static_buffer_ptr_t buffer)
            {
                if (state_ == state::closed)
                    return;
                buffer->set_size(buffer->max_size());
                auto mutable_buffer = buffer->mutable_buffer();
                sock_->async_receive_from(
                    mutable_buffer,
                    endpoint_,
                    [this, self = shared_from_this(), buffer = std::move(buffer)](const std::error_code& ec, size_t size) mutable
                {
                    if (ec)
                    {
                        close(ec);
                    }
                    else
                    {
                        on_receive(buffer->data(), size, clock());
                        do_receive(std::move(buffer));
                    }
                });
            }
        public:
            connection(udp::socket* sock, uint32_t conv, udp::endpoint& endpoint, bool isserver)
                : isserver_(isserver)
                , conv_(conv)
                , now_tick_(clock())
                , sock_(sock)
                , endpoint_(std::move(endpoint))
            {
                init_kcp_context();
            }

            void run_client()
            {
                if (!isserver_)
                {
                    if (state_ == state::idle)
                    {
                        state_ = state::opened;
                        do_receive(static_buffer_pool::make());
                        start_timer();
                    }
                }
            }

            virtual ~connection()
            {
                close(asio::error::operation_aborted);
                console_log("%s.connection destructor: %u", (isserver_ ? "server": "client" ), conv_);
                if (!isserver_) {
                    delete sock_;
                }
            }

            asio::any_io_executor get_executor()
            {
                return sock_->get_executor();
            }

            /*
            * Start an asynchronous operation to read data into a dynamic buffer sequence,
            * until it's size() >=size.
            */
            template<typename Allocator, typename ReadHandler>
            auto async_read(asio::basic_streambuf<Allocator>& b, size_t size, ReadHandler&& handler)
            {
                return asio::async_initiate<ReadHandler, void(asio::error_code, std::size_t)>(
                    initiate_async_read<false>(this), handler, asio::basic_streambuf_ref(b), size);
            }

            /* This function is used to asynchronously receive data from kcp. The function call always returns immediately.
            *
            *  Buffer size 2048 is ok.
            */
            template<typename MutableBuffer, typename ReadHandler>
            auto async_read_some(const MutableBuffer& buffer, ReadHandler&& handler)
            {
                return asio::async_initiate<ReadHandler, void(const std::error_code&, size_t)>(
                    initiate_async_read<true>(this), handler, buffer, 0);
            }

            template<typename ConstBuffer, typename WriteHandler>
            auto async_write(const ConstBuffer& buffer, WriteHandler&& handler)
            {
                return asio::async_initiate<WriteHandler, void(const std::error_code&, size_t)>(
                    initiate_async_write(this), handler, buffer);
            }

            bool raw_send(const char* data, size_t size, uint8_t opcode)
            {
                assert(size <= 2048);
                auto buffer = static_buffer_pool::make();
                buffer->write(opcode, data, size);
                auto const_buffer = buffer->const_buffer();
                if (opcode != opcode_disconnect)
                {
                    sock_->async_send_to(
                        const_buffer,
                        endpoint_,
                        [self = shared_from_this(), buffer = std::move(buffer)]
                    (const std::error_code&, size_t) {
                        //console_log("udp send size: %zu", size);
                    });
                }
                else
                {
                    sock_->send_to(const_buffer, endpoint_);
                }
                return true;
            }

            uint32_t get_conv() const
            {
                return conv_;
            }

            bool closed() const
            {
                return state_ == state::closed;
            }

            bool idle() const
            {
                return state_ == state::idle;
            }

            bool is_server() const
            {
                return isserver_;
            }

            udp::socket& get_socket()
            {
                return *sock_;
            }

            auto& endpoint() {
                return endpoint_;
            }

            void close(asio::error_code ec)
            {
                if (state_ != state::opened)
                    return;

                if (ec == asio::error::timed_out)
                {
                    console_log("connection(%s):  %u timeout", (is_server() ? "server" : "client"), conv_);
                }

                state_ = state::closed;
                ikcp_flush(obj_.get());
                check_read_op(ec);
                char addon[32] = { 0 };
                raw_send(addon, sizeof(addon), opcode_disconnect);
                if (!isserver_)
                {
                    asio::error_code ignore;
                    sock_->close(ignore);
                    if (timer_)
                        timer_->cancel();
                }
            }
        };

        using connection_ptr = std::shared_ptr<connection>;

        //------------------------------acceptor------------------------------------

        class acceptor
        {
        public:
            using endpoint_type = udp::endpoint;
            using executor_type = asio::any_io_executor ;
        private:
            class initiate_async_accept
            {
            public:
                explicit initiate_async_accept(acceptor* self)
                    : self_(self)
                {
                }

                executor_type get_executor() const
                {
                    return self_->get_executor();
                }

                template <typename AcceptHandler>
                void operator()(AcceptHandler&& handler) const
                {
                    using op = accept_op<std::decay_t<AcceptHandler>>;
                    self_->accept_ops_.push(std::make_shared<op>(std::forward<AcceptHandler>(handler)));
                }

            private:
                acceptor* self_;
            };

            template <typename Handler>
            class accept_op :public operation
            {
            public:
                accept_op(Handler&& handler)
                    :operation(&accept_op::do_complete)
                    , handler_(std::move(handler))
                {
                }

                static bool do_complete(void* p, operation* base, const asio::error_code&, std::size_t /*bytes_transferred*/)
                {
                    accept_op* o(static_cast<accept_op*>(base));
                    o->handler_(std::move(*(connection_ptr*)p));
                    return true;
                }
            private:
                Handler handler_;
            };
        public:
            acceptor(const executor_type& executor, udp::endpoint endpoint, std::string magic)
                :magic_(magic)
                , timer_(executor)
            {
                asio::co_spawn(executor, do_receive(endpoint), asio::detached);
                asio::co_spawn(executor, do_update(), asio::detached);
            }

            acceptor(const acceptor&) = delete;
            acceptor& operator=(const acceptor&) = delete;

            executor_type get_executor()
            {
                return timer_.get_executor();
            }

            template <typename AcceptHandler>
            auto async_accept(AcceptHandler&& handler)
            {
                return asio::async_initiate<AcceptHandler, void(const connection_ptr& c)>(
                    initiate_async_accept(this), handler);
            }
        private:
            asio::awaitable<void> do_update()
            {
                while (true)
                {
                    timer_.expires_after(std::chrono::milliseconds(5));
                    auto [ec]= co_await timer_.async_wait(asio::as_tuple(asio::use_awaitable));
                    if (!ec)
                    {
                        now_ = clock();
                        for (auto iter = connections_.begin(); iter != connections_.end();)
                        {
                            auto lastrecvtime = iter->second->update(now_);
                            if (iter->second->closed() || (now_ - lastrecvtime) > timeout_duration)
                            {
                                iter->second->close(iter->second->closed() ? asio::error::operation_aborted : asio::error::timed_out);
                                iter = connections_.erase(iter);
                            }
                            else
                            {
                                ++iter;
                            }
                        }
                    }
                    else
                    {
                        break;
                    }
                }
            }

            asio::awaitable<void> do_receive(udp::endpoint endpoint)
            {
                auto executor = co_await asio::this_coro::executor;
                udp::socket socket{executor, endpoint};
                udp::endpoint sender_endpoint;
                std::array<char, 2048> buffer{};

                size_t handshark_size = 1 + magic_.size() + 4;
                while (true)
                {
                    auto [ec, n] = co_await socket.async_receive_from(asio::buffer(buffer.data(), buffer.size()), sender_endpoint, asio::as_tuple(asio::use_awaitable));
                    if (!ec)
                    {
                        uint8_t opcode = buffer[0];
                        if (opcode >= opcode_max)
                            continue;

                        if (opcode == opcode_handshark)
                        {
                            if (n == handshark_size && std::string_view{buffer.data() + 1, magic_.size()} == magic_)
                            {
                                uint32_t conv = 0;
                                memcpy(&conv, buffer.data() + 1 + magic_.size(), sizeof(uint32_t));
                                asio::co_spawn(executor, handshark(socket, conv, sender_endpoint), asio::detached);
                            }
                        }
                        else
                        {
                            if (auto iter = connections_.find(sender_endpoint); iter != connections_.end())
                            {
                                iter->second->on_receive(buffer.data(), n, now_);
                            }
                            else
                            {
                                printf("warn: acceptor udp receive from %s, opcode %d \n", address(sender_endpoint).data(), int(opcode));
                            }
                        }
                    }
                    else
                    {
                        printf("acceptor udp receive from %s, error %s \n", address(sender_endpoint).data(), ec.message().data());
                    }
                }
            }

            asio::awaitable<void> handshark(udp::socket &sock, uint32_t conv, udp::endpoint endpoint)
            {
                auto executor = co_await asio::this_coro::executor;
                connection_ptr c;
                if (auto iter = connections_.find(endpoint); iter != connections_.end())
                {
                    if (iter->second->idle())
                    {
                        c = iter->second;
                    }
                    else
                    {
                        iter->second->close(asio::error::make_error_code(asio::error::operation_aborted));
                        connections_.erase(iter);
                    }
                }

                if (!c)
                {
                    c = std::make_shared<connection>(&sock, conv, endpoint, true);
                    connections_.emplace(endpoint, c);
                }

                std::unique_ptr<std::string> response = std::make_unique<std::string>();
                conv = c->get_conv();

                char data[32];
                memcpy(data, &conv, sizeof(conv));
                c->raw_send(data, sizeof(conv), opcode_handshark_response);

                std::shared_ptr<operation> op = std::move(accept_ops_.front());
                accept_ops_.pop();
                op->complete(&c, std::error_code(), 0);
            }
        private:
            uint32_t conv_ = 0;
            time_t now_ = clock();
            std::string magic_;
            asio::steady_timer timer_;
            std::queue<std::shared_ptr<operation>> accept_ops_;
            std::unordered_map<udp::endpoint, connection_ptr> connections_;
        };

        //------------------------------connector------------------------------------
        template<typename Executor>
        inline asio::awaitable<std::tuple<asio::error_code,  connection_ptr>> async_connect(const Executor& executor,  udp::endpoint endpoint, std::string magic)
        {
            thread_local static uint32_t conv = 0;
            ++conv;
            udp::socket sock(executor);
            auto [ec1] = co_await sock.async_connect(endpoint, asio::as_tuple(asio::use_awaitable));
            if (ec1) {
                co_return std::make_tuple(ec1, connection_ptr{});
            }
            std::string data;
            data.push_back((char)opcode_handshark);
            data.append(magic);
            data.append((char*)&conv, 4);
            auto [ec2, n] = co_await sock.async_send(asio::buffer(data), asio::as_tuple(asio::use_awaitable));
            if (ec2)
                co_return std::make_tuple(ec2, connection_ptr{});
            data.clear();
            data.resize(16, 0);
            auto [ec3, n2] = co_await sock.async_receive(asio::buffer(data), asio::as_tuple(asio::use_awaitable));
            if (ec3)
                co_return std::make_tuple(ec3, connection_ptr{});
            uint8_t opcode = data[0];
            if (opcode != opcode_handshark_response)
            {
                auto ec4 = moon::kcp::make_error_code(moon::kcp::error::invalid_handshark_response);
                co_return std::make_tuple(ec4, connection_ptr{});
            }
            memcpy(&conv, data.data() + 1, sizeof(conv));
            connection_ptr c = std::make_shared<connection>(new udp::socket(std::move(sock)), conv, endpoint, false);
            c->run_client();
            co_return std::make_tuple(asio::error_code{}, std::move(c));
        }
    }
}
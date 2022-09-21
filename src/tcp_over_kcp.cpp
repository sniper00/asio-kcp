#include <iostream>
#include "kcp.hpp"

using asio::ip::tcp;
using asio::ip::udp;

template<typename Socket>
static std::string address(Socket& socket)
{
    std::string address;
    asio::error_code code;
    auto endpoint = socket.remote_endpoint(code);
    if (!code)
    {
        address.append(endpoint.address().to_string());
        address.append(":");
        address.append(std::to_string(endpoint.port()));
    }
    return address;
}

struct config
{
    tcp::resolver::results_type tcp_remote_endpoint;
    tcp::endpoint tcp_local_endpoint;
    udp::endpoint udp_remote_endpoint;
    udp::endpoint udp_local_endpoint;
};

static config cfg;

constexpr uint8_t connect_key[32] = { 49, 194, 61, 231, 161, 216, 31, 60, 230, 26, 13, 25, 211, 185, 93, 67, 118, 28, 49, 220, 184, 71, 20, 228, 4, 24, 36, 205, 222, 99, 240, 152 };

constexpr size_t buffer_size = 8192;

std::string magic = std::string{(char*)connect_key, 32};

asio::awaitable<void> kcp2tcp(moon::kcp::connection_ptr kcp_socket, std::shared_ptr< tcp::socket> tcp_socket)
{
    try
    {
        std::array<char, buffer_size> buffer;
        while (true)
        {
            size_t n = co_await kcp_socket->async_read_some(asio::buffer(buffer), asio::use_awaitable);
            co_await asio::async_write(*tcp_socket, asio::buffer(buffer.data(), n), asio::use_awaitable);
        }
    }
    catch (const std::exception& ex)
    {
        moon::kcp::console_log("kcp2tcp exception: %s", ex.what());
        kcp_socket->close(asio::error::operation_aborted);
        if (tcp_socket->is_open())
        {
            asio::error_code ignore;
            tcp_socket->shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
            tcp_socket->close(ignore);
        }
    }
}

asio::awaitable<void> tcp2kcp(std::shared_ptr<tcp::socket> tcp_socket, moon::kcp::connection_ptr kcp_socket)
{
    try
    {
        std::array<char, buffer_size> buffer;
        while (true)
        {
            std::size_t n = co_await tcp_socket->async_read_some(asio::buffer(buffer), asio::use_awaitable);
            co_await kcp_socket->async_write(asio::const_buffer(buffer.data(), n), asio::use_awaitable);
        }
    }
    catch (const std::exception& ex)
    {
        moon::kcp::console_log("tcp2kcp exception: %s", ex.what());
        kcp_socket->close(asio::error::operation_aborted);
        if (tcp_socket->is_open())
        {
            asio::error_code ignore;
            tcp_socket->shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
            tcp_socket->close(ignore);
        }
    }
}

asio::awaitable<void> start_pipe(moon::kcp::connection_ptr kcp_socket)
{
    try
    {
        moon::kcp::console_log("kcp2tcp accept %s, and start tcp.async_connect", address(kcp_socket->get_socket()).data());
        std::shared_ptr< tcp::socket> tcp_socket = std::make_shared<tcp::socket>(kcp_socket->get_executor());
        co_await asio::async_connect(*tcp_socket, cfg.tcp_remote_endpoint, asio::use_awaitable);
        moon::kcp::console_log("kcp2tcp accept %s, and tcp.async_connect success", address(kcp_socket->get_socket()).data());
        asio::co_spawn(kcp_socket->get_executor(), kcp2tcp(kcp_socket, tcp_socket), asio::detached);
        asio::co_spawn(kcp_socket->get_executor(), tcp2kcp(tcp_socket, kcp_socket), asio::detached);
    }
    catch (const std::exception& ex)
    {
        moon::kcp::console_log("kcp_server tcp_async_connect exception: %s", ex.what());
    }
}

asio::awaitable<void> run_kcp_server(moon::kcp::acceptor& acceptor)
{
    try
    {
        while (true)
        {
            auto kcp_socket = co_await acceptor.async_accept(asio::use_awaitable);
            asio::co_spawn(kcp_socket->get_executor(), start_pipe(kcp_socket), asio::detached);
        }
    }
    catch (const std::exception& ex)
    {
        moon::kcp::console_log("kcp_server exception: %s", ex.what());
    }
}

asio::awaitable<void> start_pipe(std::shared_ptr<tcp::socket> tcp_socket)
{
    moon::kcp::console_log("tcp2kcp accept %s, and start kcp.async_connect", address(*tcp_socket).data());
    moon::kcp::connection_ptr kcp_socket;
    int try_times = 5;
    while (true)
    {
        try
        {
            kcp_socket = co_await moon::kcp::async_connect(tcp_socket->get_executor(), cfg.udp_remote_endpoint, magic, 500);
            break;
        }
        catch (const std::exception&)
        {
            --try_times;
        }

        if (try_times == 0)
        {
            moon::kcp::console_log("kcp connect failed: timeout ");
            break;
        }
    }

    if (kcp_socket)
    {
        moon::kcp::console_log("tcp2kcp accept %s, and kcp.async_connect success", address(*tcp_socket).data());
        asio::co_spawn(tcp_socket->get_executor(), tcp2kcp(tcp_socket, kcp_socket), asio::detached);
        asio::co_spawn(tcp_socket->get_executor(), kcp2tcp(kcp_socket, tcp_socket), asio::detached);
    }
}

asio::awaitable<void> run_tcp_server(tcp::acceptor acceptor)
{
    while (true)
    {
        try
        {
            auto tcp_sock = co_await acceptor.async_accept(asio::use_awaitable);
            std::shared_ptr< tcp::socket> tcp_socket = std::make_shared<tcp::socket>(std::move(tcp_sock));
            asio::co_spawn(tcp_socket->get_executor(), start_pipe(tcp_socket), asio::detached);
        }
        catch (const std::exception& ex)
        {
            moon::kcp::console_log("tcp acceptor exception: %s", ex.what());
        }
    }
}

void usage(void) {
    std::cout << "Usage:\n";
    std::cout << "        tcp2kcp [-m] [-l local endpoint] [-r remote endpoint]\n";
    std::cout << "The options are:\n";
    std::cout << "        -m        mode(tcp2kcp or kcp2tcp)\n";
    std::cout << "        -l          local endpoint(127.0.0.1:8000)\n";
    std::cout << "        -r          remote endpoint(127.0.0.1:8000)\n";
}

int main(int argc, char** argv)
{
    std::string mode;
    std::string local;
    std::string remote;

    if(argc<4)
    {
        usage();
        return -1;
    }

    for (int i = 1; i < argc; ++i)
    {
        bool lastarg = i == (argc - 1);
        std::string_view v{ argv[i] };
        if ((v == "-h" || v == "--help") && !lastarg)
        {
            usage();
            return -1;
        }
        else if ((v == "-m" || v == "--mode") && !lastarg)
        {
            mode = argv[++i];
        }
        else if ((v == "-l" || v == "--local") && !lastarg)
        {
            local = argv[++i];
        }
        else if ((v == "-r" || v == "--remote") && !lastarg)
        {
            remote = argv[++i];
        }
        else
        {
            usage();
            return -1;
        }
    }

    moon::kcp::console_log("start with mode:  %s   listen: %s remote: %s",mode.data(), local.data(), remote.data());

    try
    {
        asio::io_context ioctx;

        asio::signal_set signals(ioctx, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { ioctx.stop(); });

        {
            auto [host, port] = moon::kcp::parse_host_port(local, 0);
            asio::ip::tcp::resolver resolver(ioctx.get_executor());
            cfg.tcp_local_endpoint = *resolver.resolve(host, std::to_string(port)).begin();
        }

        {
            auto [host, port] = moon::kcp::parse_host_port(remote, 0);
            asio::ip::tcp::resolver resolver(ioctx.get_executor());
            cfg.tcp_remote_endpoint = resolver.resolve(host, std::to_string(port));
        }

        {
            auto [host, port] = moon::kcp::parse_host_port(local, 0);
            asio::ip::udp::resolver resolver(ioctx.get_executor());
            cfg.udp_local_endpoint = *resolver.resolve(host, std::to_string(port)).begin();
        }

        {
            auto [host, port] = moon::kcp::parse_host_port(remote, 0);
            asio::ip::udp::resolver resolver(ioctx.get_executor());
            cfg.udp_remote_endpoint = *resolver.resolve(host, std::to_string(port)).begin();
        }

        if (mode == "kcp2tcp")
        {
            moon::kcp::acceptor acceptor{ ioctx.get_executor(), cfg.udp_local_endpoint, magic };
            asio::co_spawn(ioctx, run_kcp_server(acceptor), asio::detached);
            ioctx.run();
        }
        else
        {
            asio::co_spawn(ioctx, run_tcp_server(tcp::acceptor(ioctx, cfg.tcp_local_endpoint)), asio::detached);
            ioctx.run();
        }
    }
    catch (const std::exception& ex)
    {
        moon::kcp::console_log("main exception:  %s", ex.what());
    }

    return 0;
}
# asio-kcp
Modern use kcp with asio, support async_accept, async_connect, async_read, async_read_some and can use with asio coroutine.

# Example

see `tcp_over_kcp.cpp`, a demo can convert tcp stream data to kcp packet or convert kcp packet data to tcp stream data. it worked in 2 modes:
- tcp2kcp: listen tcp endpoint and read data then use kcp send it.
- kcp2tcp: listen udp endpoint and read data then use tcp send it.

```shell
tcp_over_kcp.exe -m tcp2kcp -l "0.0.0.0:6001" -r "127.0.0.1:6002"
tcp_over_kcp.exe -m kcp2tcp -l "127.0.0.1:6002" -r "github.com:443"
```

# Sneak Peek

- async_accept
```cpp
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
```

- async_read_some
```cpp
asio::awaitable<void> kcp2tcp(moon::kcp::connection_ptr kcp_socket, std::shared_ptr< tcp::socket> tcp_socket)
{
    try
    {
        std::array<char, 2048> buffer;
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
```
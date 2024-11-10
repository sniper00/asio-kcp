#pragma once
#include<system_error>

namespace moon
{
    namespace kcp
    {
        enum class error;
        enum class condition;
    }
}

namespace std {
    template<>
    struct is_error_code_enum<moon::kcp::error>
    {
        static bool const value = true;
    };
    template<>
    struct is_error_condition_enum<moon::kcp::condition>
    {
        static bool const value = true;
    };
} // std

namespace moon
{
    namespace kcp
    {
        enum class error
        {
            invalid_handshark_response = 900000,
            connect_timeout,
        };

        /// Error conditions corresponding to sets of error codes.
        enum class condition
        {
            kcp_handshake_failed = 1,
        };

        namespace detail
        {
            class error_codes : public std::error_category
            {
            public:
                const char* name() const noexcept override
                {
                    return "moon.socket.kcp";
                }

                std::string message(int ev) const override
                {
                    switch (static_cast<error>(ev))
                    {
                    default:
                    case error::invalid_handshark_response:  return "invalid_handshark_response";
                    case error::connect_timeout: return "connect timeout";
                    }
                }

                std::error_condition default_error_condition(int ev) const noexcept override
                {
                    switch (static_cast<error>(ev))
                    {
                    default:
                    case error::invalid_handshark_response:
                    case error::connect_timeout:
                        return { ev, *this };
                    }
                }
            };

            class error_conditions : public std::error_category
            {
            public:
                const char* name() const noexcept override
                {
                    return "moon.socket.kcp";
                }

                std::string message(int cv) const override
                {
                    switch (static_cast<condition>(cv))
                    {
                    default:
                    case condition::kcp_handshake_failed: return "The kcp handshake failed";
                    }
                }
            };
        }

        inline std::error_code
            make_error_code(error e)
        {
            static detail::error_codes const cat{};
            return std::error_code{ static_cast<
                std::underlying_type<error>::type>(e), cat };
        }

        inline std::error_condition
            make_error_condition(condition c)
        {
            static detail::error_conditions const cat{};
            return std::error_condition{ static_cast<
                std::underlying_type<condition>::type>(c), cat };
        }
    }
}
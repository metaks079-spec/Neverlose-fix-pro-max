/*
	This is what you think.
*/
#ifndef NEVERLOSE_SDK_HPP
#define NEVERLOSE_SDK_HPP
#include <cstdint>
#include <cstring>
#include <string>

// For C++14 compatibility, provide string_view
#if defined(__cpp_lib_string_view) || __cplusplus >= 201703L
    #include <string_view>
#else
    // Check if string_view is already defined
    #if !defined(_STRING_VIEW_) && !defined(_LIBCPP_STRING_VIEW)
        // Minimal string_view polyfill for C++14
        namespace std {
            class string_view {
                const char* data_;
                size_t size_;
            public:
                constexpr string_view() : data_(nullptr), size_(0) {}
                constexpr string_view(const char* str, size_t len) : data_(str), size_(len) {}
                string_view(const char* str) : data_(str), size_(str ? std::strlen(str) : 0) {}
                string_view(const std::string& str) : data_(str.data()), size_(str.size()) {}
                
                constexpr const char* data() const { return data_; }
                constexpr size_t size() const { return size_; }
                constexpr size_t length() const { return size_; }
                constexpr bool empty() const { return size_ == 0; }
            };
        }
    #endif
#endif

#include "json.hpp"

namespace neverlosesdk
{
	namespace network
	{
		class Requestor
		{
			virtual void MakeRequest(std::string& out, std::string_view route, int, int) = 0;
			virtual void GetSerial(std::string& out, nlohmann::json& request) = 0;
			virtual void fn2() = 0;
			// fn3 is the WebSocket data handler - same signature as GetSerial
			// Called for: config fetches (type 0), heartbeat (type 1), skin data (type 2),
			//             entity/netvar data (type 3), auth (type 4), lua scripts (type 5)
			virtual void fn3(std::string& out, nlohmann::json& request) = 0;
			virtual void QueryLuaLibrary(std::string& out, std::string_view name) = 0;
		};

		class Client
		{
			virtual void vt() = 0;

			int IsConnected;
			void* endpoint; // websocketpp object
			uint32_t resrved[0x2];
			char* SomeKey; // Message from auth
			uint32_t resrved2[0x6];
			char* SomeKey1; // Data from auth
		};
	};

	namespace gui
	{
		class Menu
		{
		public:
			char pad0[0x4];
			bool IsOpen;
			char pad1[0x3];
			float Alpha;
			int IsEditingStyle; // normaly -1, when edit style popup visible turns 0
			char pad2[0x8];
		};
	};
};

#endif // NEVERLOSE_SDK_HPP
/*
 * Copyright 2010-2011 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#ifndef __BNET_P_H__
#define __BNET_P_H__

#include "bnet.h"

#define BNET_CONFIG_DEBUG 0
extern void dbgPrintf(const char* _format, ...);
extern void dbgPrintfData(const void* _data, uint32_t _size, const char* _format, ...);

#if BNET_CONFIG_DEBUG
#	define BX_TRACE(_format, ...) \
				do { \
					dbgPrintf(BX_FILE_LINE_LITERAL "BNET " _format "\n", ##__VA_ARGS__); \
				} while(0)

#	define BX_CHECK(_condition, _format, ...) \
				do { \
					if (!(_condition) ) \
					{ \
						BX_TRACE(BX_FILE_LINE_LITERAL _format, ##__VA_ARGS__); \
						bx::debugBreak(); \
					} \
				} while(0)
#endif // 0

#define BX_NAMESPACE 1
#include <bx/bx.h>

#ifndef BNET_CONFIG_OPENSSL
#	define BNET_CONFIG_OPENSSL (BX_PLATFORM_WINDOWS && BX_COMPILER_MSVC) || BX_PLATFORM_ANDROID
#endif // BNET_CONFIG_OPENSSL

#ifndef BNET_CONFIG_DEBUG
#	define BNET_CONFIG_DEBUG 0
#endif // BNET_CONFIG_DEBUG

#ifndef BNET_CONFIG_CONNECT_TIMEOUT_SECONDS
#	define BNET_CONFIG_CONNECT_TIMEOUT_SECONDS 5
#endif // BNET_CONFIG_CONNECT_TIMEOUT_SECONDS

#ifndef BNET_CONFIG_MAX_INCOMING_BUFFER_SIZE
#	define BNET_CONFIG_MAX_INCOMING_BUFFER_SIZE (64<<10)
#endif // BNET_CONFIG_MAX_INCOMING_BUFFER_SIZE

#if BX_PLATFORM_WINDOWS || BX_PLATFORM_XBOX360
#	if BX_PLATFORM_WINDOWS
#		define _WIN32_WINNT 0x0501
#		include <winsock2.h>
#		include <ws2tcpip.h>
#	elif BX_PLATFORM_XBOX360
#		include <xtl.h>
#	endif
#	define socklen_t int32_t
#	define EWOULDBLOCK WSAEWOULDBLOCK
#	define EINPROGRESS WSAEINPROGRESS
#elif BX_PLATFORM_LINUX || BX_PLATFORM_ANDROID
#	include <memory.h>
#	include <errno.h> // errno
#	include <fcntl.h>
#	include <netdb.h>
#	include <unistd.h>
#	include <sys/socket.h>
#	include <sys/time.h> // gettimeofday
#	include <arpa/inet.h> // inet_addr
#	include <netinet/in.h>
#	include <netinet/tcp.h>
	typedef int SOCKET;
	typedef linger LINGER;
	typedef hostent HOSTENT;
	typedef in_addr IN_ADDR;
	
#	define SOCKET_ERROR (-1)
#	define INVALID_SOCKET (-1)
#	define closesocket close
#elif BX_PLATFORM_NACL
#	include <errno.h> // errno
#	include <stdio.h> // sscanf
#	include <string.h>
#	include <sys/time.h> // gettimeofday
#	include <sys/types.h> // fd_set
#	include "nacl_socket.h"
#endif // BX_PLATFORM_

#include <bx/debug.h>
#include <bx/blockalloc.h>
#include <bx/ringbuffer.h>
#include <bx/timer.h>

#include <new> // placement new

#if BNET_CONFIG_OPENSSL
#	include <openssl/err.h>
#	include <openssl/ssl.h>
#else
#	define SSL_CTX void
#	define X509 void
#	define EVP_PKEY void
#endif // BNET_CONFIG_OPENSSL

#include <list>

namespace bnet
{
	extern reallocFn g_realloc;
	extern freeFn g_free;

	struct Internal
	{
		enum Enum
		{
			None,
			Disconnect,
			Notify,

			Count,
		};
	};

	uint16_t ctxAccept(uint16_t _listenHandle, SOCKET _socket, uint32_t _ip, uint16_t _port, bool _raw, X509* _cert, EVP_PKEY* _key);
	void ctxPush(uint16_t _handle, MessageId::Enum _id);
	void ctxPush(Message* _msg);
	Message* ctxAlloc(uint16_t _handle, uint16_t _size, bool _incoming = false, Internal::Enum _type = Internal::None);
	void ctxFree(Message* _msg);

	template<typename Ty> class FreeList
	{
	public:
		FreeList(uint16_t _max)
		{
			uint32_t size = BlockAlloc::minElementSize > sizeof(Ty) ? BlockAlloc::minElementSize : sizeof(Ty);
			m_memBlock = g_realloc(NULL, _max*size);
			m_allocator = BlockAlloc(m_memBlock, _max, size);
		}

		~FreeList()
		{
			g_free(m_memBlock);
		}

		uint16_t getIndex(Ty* _obj) const
		{
			return m_allocator.getIndex(_obj);
		}

		Ty* create()
		{
			Ty* obj = static_cast<Ty*>(m_allocator.alloc() );
			obj = ::new (obj) Ty;
			return obj;
		}

		template<typename Arg0> Ty* create(Arg0 _a0)
		{
			Ty* obj = static_cast<Ty*>(m_allocator.alloc() );
			obj = ::new (obj) Ty(_a0);
			return obj;
		}

		template<typename Arg0, typename Arg1> Ty* create(Arg0 _a0, Arg1 _a1)
		{
			Ty* obj = static_cast<Ty*>(m_allocator.alloc() );
			obj = ::new (obj) Ty(_a0, _a1);
			return obj;
		}

		template<typename Arg0, typename Arg1, typename Arg2> Ty* create(Arg0 _a0, Arg1 _a1, Arg2 _a2)
		{
			Ty* obj = static_cast<Ty*>(m_allocator.alloc() );
			obj = ::new (obj) Ty(_a0, _a1, _a2);
			return obj;
		}

		void destroy(Ty* _obj)
		{
			_obj->~Ty();
			m_allocator.free(_obj);
		}

		Ty* getFromIndex(uint16_t _index)
		{
			Ty* obj = static_cast<Ty*>(m_allocator.getFromIndex(_index) );
			return obj;
		}

	private:
		void* m_memBlock;
		BlockAlloc m_allocator;
	};

	class RecvRingBuffer
	{
	public:
		RecvRingBuffer(RingBufferControl& _control, char* _buffer)
			: m_control(_control)
			, m_write(_control.m_current)
			, m_reserved(0)
			, m_buffer(_buffer)
		{
		}

		~RecvRingBuffer()
		{
		}

		int recv(SOCKET _socket)
		{
			m_reserved += m_control.reserve(-1);
			uint32_t end = (m_write + m_reserved) % m_control.m_size;
			uint32_t wrap = end < m_write ? m_control.m_size - m_write : m_reserved;
			char* to = &m_buffer[m_write];

			int bytes = ::recv(_socket
							  , to
							  , wrap
							  , 0
							  );

			if (0 < bytes)
			{
				m_write += bytes;
				m_write %= m_control.m_size;
				m_reserved -= bytes;
				m_control.commit(bytes);
			}

			return bytes;
		}

#if BNET_CONFIG_OPENSSL
		int recv(SSL* _ssl)
		{
			m_reserved += m_control.reserve(-1);
			uint32_t end = (m_write + m_reserved) % m_control.m_size;
			uint32_t wrap = end < m_write ? m_control.m_size - m_write : m_reserved;
			char* to = &m_buffer[m_write];

			int bytes = SSL_read(_ssl
								, to
								, wrap
								);

			if (0 < bytes)
			{
				m_write += bytes;
				m_write %= m_control.m_size;
				m_reserved -= bytes;
				m_control.commit(bytes);
			}

			return bytes;
		}
#endif // BNET_CONFIG_OPENSSL

	private:
		RecvRingBuffer();
		RecvRingBuffer(const RecvRingBuffer&);
		void operator=(const RecvRingBuffer&);

		RingBufferControl& m_control;
		uint32_t m_write;
		uint32_t m_reserved;
		char* m_buffer;
	};

	class MessageQueue
	{
	public:
		MessageQueue()
		{
		}

		~MessageQueue()
		{
		}

		void push(Message* _msg)
		{
			m_queue.push_back(_msg);
		}

		Message* peek()
		{
			if (!m_queue.empty() )
			{
				return m_queue.front();
			}

			return NULL;
		}

		Message* pop()
		{
			if (!m_queue.empty() )
			{
				Message* msg = m_queue.front();
				m_queue.pop_front();
				return msg;
			}

			return NULL;
		}

	private:
		std::list<Message*> m_queue;
	};

} // namespace bnet

#endif // __BNET_P_H__
/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_SOCKET_H
#define _LIBBOUNCE_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//////////////////////////////////////////////////////////////////////////////////

#if defined(BOUNCE_POSIX) || defined(BOUNCE_POSIX_GLIB)

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize a socket I/O operation.
 * @param operation Socket I/O operation storage provided by the caller.
 * @remarks Only one operation may be active per storage object.
 */
extern void bounce_socket_io_init(BOUNCE_SOCKET_IO *operation);

/**
 * @brief Deinitialize a socket I/O operation.
 * @param operation Socket I/O operation storage provided by the caller.
 * @remarks The operation must not be active when this function is called.
 */
extern void bounce_socket_io_deinit(BOUNCE_SOCKET_IO *operation);

/**
 * @brief Get the socket I/O operation result.
 * @param operation Initialized socket I/O operation storage.
 * @return Bytes sent/received, zero for EOF or zero-length operations, or -1
 * when the underlying socket operation reported an error.
 */
extern int64_t bounce_socket_io_result(const BOUNCE_SOCKET_IO *operation);

/**
 * @brief Get the socket I/O operation error code.
 * @param operation Initialized socket I/O operation storage.
 * @return POSIX errno value. Returns zero when the operation result is not an
 * error.
 */
extern int bounce_socket_io_error(const BOUNCE_SOCKET_IO *operation);

/**
 * @brief Check whether a socket I/O operation is currently active.
 * @param operation Initialized socket I/O operation storage.
 * @return True when an operation is active.
 */
extern bool bounce_socket_io_active(const BOUNCE_SOCKET_IO *operation);

/**
 * @brief Await a socket receive operation.
 * @param r Initialized BOUNCE_CORE.
 * @param operation Initialized socket I/O operation storage.
 * @param fd Socket file descriptor to receive from.
 * @param buffer Destination buffer.
 * @param length Maximum bytes to receive.
 * @param flags Flags passed to `recv()`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when local setup succeeded.
 */
extern bool bounce_await_socket_recv(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Await a socket send operation.
 * @param r Initialized BOUNCE_CORE.
 * @param operation Initialized socket I/O operation storage.
 * @param fd Socket file descriptor to send to.
 * @param buffer Source buffer.
 * @param length Maximum bytes to send.
 * @param flags Flags passed to `send()`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when local setup succeeded.
 * @remarks Partial sends complete successfully with the number of bytes sent.
 */
extern bool bounce_await_socket_send(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Await a socket receive-from operation.
 * @param r Initialized BOUNCE_CORE.
 * @param operation Initialized socket I/O operation storage.
 * @param fd Socket file descriptor to receive from.
 * @param buffer Destination buffer.
 * @param length Maximum bytes to receive.
 * @param flags Flags passed to `recvfrom()`.
 * @param address Optional peer address buffer.
 * @param address_length Optional peer address length pointer. Required when
 * address is not `NULL`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when local setup succeeded.
 */
extern bool bounce_await_socket_recvfrom(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  struct sockaddr *address,
  socklen_t *address_length,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Await a socket send-to operation.
 * @param r Initialized BOUNCE_CORE.
 * @param operation Initialized socket I/O operation storage.
 * @param fd Socket file descriptor to send to.
 * @param buffer Source buffer.
 * @param length Maximum bytes to send.
 * @param flags Flags passed to `sendto()`.
 * @param address Optional destination address.
 * @param address_length Destination address length.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when local setup succeeded.
 * @remarks Partial sends complete successfully with the number of bytes sent.
 */
extern bool bounce_await_socket_sendto(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  const struct sockaddr *address,
  socklen_t address_length,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Await a socket receive-message operation.
 * @param r Initialized BOUNCE_CORE.
 * @param operation Initialized socket I/O operation storage.
 * @param fd Socket file descriptor to receive from.
 * @param message Message buffer passed to `recvmsg()`.
 * @param flags Flags passed to `recvmsg()`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when local setup succeeded.
 */
extern bool bounce_await_socket_recvmsg(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  struct msghdr *message,
  int flags,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Await a socket send-message operation.
 * @param r Initialized BOUNCE_CORE.
 * @param operation Initialized socket I/O operation storage.
 * @param fd Socket file descriptor to send to.
 * @param message Message buffer passed to `sendmsg()`.
 * @param flags Flags passed to `sendmsg()`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when local setup succeeded.
 * @remarks Partial sends complete successfully with the number of bytes sent.
 */
extern bool bounce_await_socket_sendmsg(
  BOUNCE_CORE *r,
  BOUNCE_SOCKET_IO *operation,
  int fd,
  const struct msghdr *message,
  int flags,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

#ifdef __cplusplus
}
#endif

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
namespace libbounce {

class bounce;
class bounce_ref;

/**
 * @brief Caller-owned socket I/O operation storage for the C++ helper API.
 * @tparam TBOUNCE_CORE Backend bounce core storage type.
 * @tparam TBOUNCE_SOCKET_IO Backend socket I/O operation storage type.
 */
template <typename TBOUNCE_CORE, typename TBOUNCE_SOCKET_IO> class socket_io_base {
private:
  TBOUNCE_SOCKET_IO operation_;
  socket_io_base(const socket_io_base&) = delete;
  socket_io_base(socket_io_base&&) = delete;
  socket_io_base& operator=(const socket_io_base&) = delete;
  socket_io_base& operator=(socket_io_base&&) = delete;

protected:
  template<typename COMPLETION_TYPE>
  static inline void callable_completion(
    BOUNCE_COMPLETION_RESULT result,
    void *completion_state) noexcept {
    bounce_base<TBOUNCE_CORE>::template callable_completion<COMPLETION_TYPE>(
      result,
      completion_state);
  }

  inline socket_io_base() noexcept {
    ::bounce_socket_io_init(&operation_);
  }

  inline ~socket_io_base() noexcept {
    ::bounce_socket_io_deinit(&operation_);
  }

public:
  /**
   * @brief Get the underlying socket I/O operation storage.
   * @return Backend socket I/O operation storage pointer.
   */
  inline TBOUNCE_SOCKET_IO *get_socket_io() noexcept {
    return &operation_;
  }

  /**
   * @brief Get the last operation result.
   * @return Bytes sent/received, EOF zero, zero-length result, or -1.
   */
  inline int64_t result() const noexcept {
    return ::bounce_socket_io_result(&operation_);
  }

  /**
   * @brief Get the last operation error code.
   * @return POSIX errno value, or zero when there was no syscall error.
   */
  inline int error() const noexcept {
    return ::bounce_socket_io_error(&operation_);
  }

  /**
   * @brief Check whether this operation storage is active.
   * @return True when an operation is active.
   */
  inline bool active() const noexcept {
    return ::bounce_socket_io_active(&operation_);
  }

  /**
   * @brief Await a socket receive operation through a bounce handle.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool recv(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    void *buffer,
    size_t length,
    int flags,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_socket_recv(
               core,
               &operation_,
               fd,
               buffer,
               length,
               flags,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a socket receive operation with a C++ callable completion.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool recv(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    void *buffer,
    size_t length,
    int flags,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type SOCKET_COMPLETION_TYPE;
    std::unique_ptr<SOCKET_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new SOCKET_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!recv(
          bounce_handle,
          fd,
          buffer,
          length,
          flags,
          &callable_completion<SOCKET_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Await a socket send operation through a bounce handle.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool send(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    const void *buffer,
    size_t length,
    int flags,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_socket_send(
               core,
               &operation_,
               fd,
               buffer,
               length,
               flags,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a socket send operation with a C++ callable completion.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool send(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    const void *buffer,
    size_t length,
    int flags,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type SOCKET_COMPLETION_TYPE;
    std::unique_ptr<SOCKET_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new SOCKET_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!send(
          bounce_handle,
          fd,
          buffer,
          length,
          flags,
          &callable_completion<SOCKET_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Await a socket receive-from operation through a bounce handle.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool recv_from(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    void *buffer,
    size_t length,
    int flags,
    struct sockaddr *address,
    socklen_t *address_length,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_socket_recvfrom(
               core,
               &operation_,
               fd,
               buffer,
               length,
               flags,
               address,
               address_length,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a socket receive-from operation with a C++ callable completion.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool recv_from(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    void *buffer,
    size_t length,
    int flags,
    struct sockaddr *address,
    socklen_t *address_length,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type SOCKET_COMPLETION_TYPE;
    std::unique_ptr<SOCKET_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new SOCKET_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!recv_from(
          bounce_handle,
          fd,
          buffer,
          length,
          flags,
          address,
          address_length,
          &callable_completion<SOCKET_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Await a socket send-to operation through a bounce handle.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool send_to(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    const void *buffer,
    size_t length,
    int flags,
    const struct sockaddr *address,
    socklen_t address_length,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_socket_sendto(
               core,
               &operation_,
               fd,
               buffer,
               length,
               flags,
               address,
               address_length,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a socket send-to operation with a C++ callable completion.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool send_to(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    const void *buffer,
    size_t length,
    int flags,
    const struct sockaddr *address,
    socklen_t address_length,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type SOCKET_COMPLETION_TYPE;
    std::unique_ptr<SOCKET_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new SOCKET_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!send_to(
          bounce_handle,
          fd,
          buffer,
          length,
          flags,
          address,
          address_length,
          &callable_completion<SOCKET_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Await a socket receive-message operation through a bounce handle.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool recv_msg(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    struct msghdr *message,
    int flags,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_socket_recvmsg(
               core,
               &operation_,
               fd,
               message,
               flags,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a socket receive-message operation with a C++ callable completion.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool recv_msg(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    struct msghdr *message,
    int flags,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type SOCKET_COMPLETION_TYPE;
    std::unique_ptr<SOCKET_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new SOCKET_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!recv_msg(
          bounce_handle,
          fd,
          message,
          flags,
          &callable_completion<SOCKET_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Await a socket send-message operation through a bounce handle.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool send_msg(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    const struct msghdr *message,
    int flags,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_socket_sendmsg(
               core,
               &operation_,
               fd,
               message,
               flags,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a socket send-message operation with a C++ callable completion.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool send_msg(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    const struct msghdr *message,
    int flags,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type SOCKET_COMPLETION_TYPE;
    std::unique_ptr<SOCKET_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new SOCKET_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!send_msg(
          bounce_handle,
          fd,
          message,
          flags,
          &callable_completion<SOCKET_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }
};

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
promise<socket_io_result> recv_async(
  bounce &bounce_handle,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> recv_async(
  bounce_ref bounce_handle,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> recv_async(
  int fd,
  void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> send_async(
  bounce &bounce_handle,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> send_async(
  bounce_ref bounce_handle,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> send_async(
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> recv_from_async(
  bounce &bounce_handle,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  struct sockaddr *address,
  socklen_t *address_length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> recv_from_async(
  bounce_ref bounce_handle,
  int fd,
  void *buffer,
  size_t length,
  int flags,
  struct sockaddr *address,
  socklen_t *address_length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> recv_from_async(
  int fd,
  void *buffer,
  size_t length,
  int flags,
  struct sockaddr *address,
  socklen_t *address_length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> send_to_async(
  bounce &bounce_handle,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  const struct sockaddr *address,
  socklen_t address_length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> send_to_async(
  bounce_ref bounce_handle,
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  const struct sockaddr *address,
  socklen_t address_length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> send_to_async(
  int fd,
  const void *buffer,
  size_t length,
  int flags,
  const struct sockaddr *address,
  socklen_t address_length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> recv_msg_async(
  bounce &bounce_handle,
  int fd,
  struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> recv_msg_async(
  bounce_ref bounce_handle,
  int fd,
  struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> recv_msg_async(
  int fd,
  struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> send_msg_async(
  bounce &bounce_handle,
  int fd,
  const struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> send_msg_async(
  bounce_ref bounce_handle,
  int fd,
  const struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);

promise<socket_io_result> send_msg_async(
  int fd,
  const struct msghdr *message,
  int flags,
  BOUNCE_CANCELLATION *cancellation = nullptr);
#endif

}
#endif

//////////////////////////////////////////////////////////////////////////////////

#endif

#endif

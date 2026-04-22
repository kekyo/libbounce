/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_FILE_H
#define _LIBBOUNCE_FILE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Use the backend's current file offset for file read/write helpers.
 * @remarks Backends that support current-offset read/write map this sentinel
 * to the native current-position operation. Backends that require explicit
 * offsets reject this value with a native invalid-argument error.
 */
#define BOUNCE_FILE_OFFSET_CURRENT INT64_C(-1)

/**
 * @brief File flush mode.
 */
typedef enum BOUNCE_FILE_FLUSH_MODE {
  BOUNCE_FILE_FLUSH_FULL = 0,
  BOUNCE_FILE_FLUSH_DATA = 1
} BOUNCE_FILE_FLUSH_MODE;

//////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
namespace libbounce {

/**
 * @brief Caller-owned file I/O operation storage for the C++ helper API.
 * @tparam TBOUNCE_CORE Backend bounce core storage type.
 * @tparam TBOUNCE_FILE_IO Backend file I/O operation storage type.
 * @tparam THANDLE Backend native file handle type.
 */
template <
  typename TBOUNCE_CORE,
  typename TBOUNCE_FILE_IO,
  typename THANDLE>
class file_io_base {
private:
  TBOUNCE_FILE_IO operation_;
  file_io_base(const file_io_base&) = delete;
  file_io_base(file_io_base&&) = delete;
  file_io_base& operator=(const file_io_base&) = delete;
  file_io_base& operator=(file_io_base&&) = delete;

protected:
  template<typename COMPLETION_TYPE>
  static inline void callable_completion(
    BOUNCE_COMPLETION_RESULT result,
    void *completion_state) noexcept {
    bounce_base<TBOUNCE_CORE>::template callable_completion<COMPLETION_TYPE>(
      result,
      completion_state);
  }

  inline file_io_base() noexcept {
    bounce_file_io_init(&operation_);
  }

  inline ~file_io_base() noexcept {
    bounce_file_io_deinit(&operation_);
  }

public:
  /**
   * @brief Get the underlying file I/O operation storage.
   * @return Backend file I/O operation storage pointer.
   */
  inline TBOUNCE_FILE_IO *get_file_io() noexcept {
    return &operation_;
  }

  /**
   * @brief Get the last operation result.
   * @return Bytes read/written, resulting seek offset, flush result, or -1.
   */
  inline int64_t result() const noexcept {
    return bounce_file_io_result(&operation_);
  }

  /**
   * @brief Get the last operation error code.
   * @return Backend native error code, or zero when there was no operation error.
   */
  inline int error() const noexcept {
    return bounce_file_io_error(&operation_);
  }

  /**
   * @brief Check whether this operation storage is active.
   * @return True when an operation is active.
   */
  inline bool active() const noexcept {
    return bounce_file_io_active(&operation_);
  }

  /**
   * @brief Await a file read operation through a bounce handle.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param handle Backend native file handle.
   * @param buffer Destination buffer.
   * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`
   * when supported by the backend.
   * @param length Maximum bytes to read.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool read(
    TBOUNCE_HANDLE &bounce_handle,
    THANDLE handle,
    void *buffer,
    int64_t offset,
    size_t length,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             bounce_await_file_read(
               core,
               &operation_,
               handle,
               buffer,
               offset,
               length,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a file read operation with a C++ callable completion.
   * @tparam TBOUNCE_HANDLE Bounce handle type exposing `get_core()`.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param handle Backend native file handle.
   * @param buffer Destination buffer.
   * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`
   * when supported by the backend.
   * @param length Maximum bytes to read.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   * @remarks Exceptions must not escape the callable.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool read(
    TBOUNCE_HANDLE &bounce_handle,
    THANDLE handle,
    void *buffer,
    int64_t offset,
    size_t length,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type FILE_COMPLETION_TYPE;
    std::unique_ptr<FILE_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new FILE_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!read(
          bounce_handle,
          handle,
          buffer,
          offset,
          length,
          &callable_completion<FILE_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Await a file write operation through a bounce handle.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param handle Backend native file handle.
   * @param buffer Source buffer.
   * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`
   * when supported by the backend.
   * @param length Maximum bytes to write.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool write(
    TBOUNCE_HANDLE &bounce_handle,
    THANDLE handle,
    const void *buffer,
    int64_t offset,
    size_t length,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             bounce_await_file_write(
               core,
               &operation_,
               handle,
               buffer,
               offset,
               length,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a file write operation with a C++ callable completion.
   * @tparam TBOUNCE_HANDLE Bounce handle type exposing `get_core()`.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param handle Backend native file handle.
   * @param buffer Source buffer.
   * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`
   * when supported by the backend.
   * @param length Maximum bytes to write.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   * @remarks Exceptions must not escape the callable.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool write(
    TBOUNCE_HANDLE &bounce_handle,
    THANDLE handle,
    const void *buffer,
    int64_t offset,
    size_t length,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type FILE_COMPLETION_TYPE;
    std::unique_ptr<FILE_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new FILE_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!write(
          bounce_handle,
          handle,
          buffer,
          offset,
          length,
          &callable_completion<FILE_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Await a file seek operation through a bounce handle.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param handle Backend native file handle.
   * @param offset Offset passed to the backend seek operation.
   * @param whence Seek base passed to the backend seek operation.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool seek(
    TBOUNCE_HANDLE &bounce_handle,
    THANDLE handle,
    int64_t offset,
    int whence,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             bounce_await_file_seek(
               core,
               &operation_,
               handle,
               offset,
               whence,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a file seek operation with a C++ callable completion.
   * @tparam TBOUNCE_HANDLE Bounce handle type exposing `get_core()`.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param handle Backend native file handle.
   * @param offset Offset passed to the backend seek operation.
   * @param whence Seek base passed to the backend seek operation.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   * @remarks Exceptions must not escape the callable.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool seek(
    TBOUNCE_HANDLE &bounce_handle,
    THANDLE handle,
    int64_t offset,
    int whence,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type FILE_COMPLETION_TYPE;
    std::unique_ptr<FILE_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new FILE_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!seek(
          bounce_handle,
          handle,
          offset,
          whence,
          &callable_completion<FILE_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }

  /**
   * @brief Await a file flush operation through a bounce handle.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param handle Backend native file handle.
   * @param mode Full metadata flush or data-only flush.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool flush(
    TBOUNCE_HANDLE &bounce_handle,
    THANDLE handle,
    BOUNCE_FILE_FLUSH_MODE mode,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             bounce_await_file_flush(
               core,
               &operation_,
               handle,
               mode,
               completion,
               completion_state,
               cancellation) :
             false;
  }

  /**
   * @brief Await a file flush operation with a C++ callable completion.
   * @tparam TBOUNCE_HANDLE Bounce handle type exposing `get_core()`.
   * @tparam COMPLETION_TYPE Callable type. Must be invocable with no arguments
   * or `BOUNCE_COMPLETION_RESULT`.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param handle Backend native file handle.
   * @param mode Full metadata flush or data-only flush.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   * @remarks Exceptions must not escape the callable.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool flush(
    TBOUNCE_HANDLE &bounce_handle,
    THANDLE handle,
    BOUNCE_FILE_FLUSH_MODE mode,
    COMPLETION_TYPE&& completion,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    typedef typename std::decay<COMPLETION_TYPE>::type FILE_COMPLETION_TYPE;
    std::unique_ptr<FILE_COMPLETION_TYPE> completion_state;

    try {
      completion_state.reset(
        new FILE_COMPLETION_TYPE(std::forward<COMPLETION_TYPE>(completion)));
    } catch (...) {
      return false;
    }

    if (!flush(
          bounce_handle,
          handle,
          mode,
          &callable_completion<FILE_COMPLETION_TYPE>,
          completion_state.get(),
          cancellation)) {
      return false;
    }

    (void)completion_state.release();
    return true;
  }
};

}
#endif

#endif

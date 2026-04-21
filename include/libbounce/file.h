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

#if defined(BOUNCE_POSIX)

/**
 * @brief Use the file descriptor's current offset for file read/write helpers.
 * @remarks Positioned reads and writes use the supplied non-negative offset.
 * Passing this sentinel makes the POSIX backend call `read()` / `write()`
 * instead of `pread()` / `pwrite()`. Linux io_uring maps this value to the
 * kernel's current-offset operation.
 */
#define BOUNCE_FILE_OFFSET_CURRENT INT64_C(-1)

/**
 * @brief File flush mode.
 */
typedef enum BOUNCE_FILE_FLUSH_MODE {
  BOUNCE_FILE_FLUSH_FULL = 0,
  BOUNCE_FILE_FLUSH_DATA = 1
} BOUNCE_FILE_FLUSH_MODE;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize a file I/O operation.
 * @param operation File I/O operation storage provided by the caller.
 * @remarks Only one operation may be active per storage object.
 */
extern void bounce_file_io_init(BOUNCE_FILE_IO *operation);

/**
 * @brief Deinitialize a file I/O operation.
 * @param operation File I/O operation storage provided by the caller.
 * @remarks The operation must not be active when this function is called.
 */
extern void bounce_file_io_deinit(BOUNCE_FILE_IO *operation);

/**
 * @brief Get the file I/O operation result.
 * @param operation Initialized file I/O operation storage.
 * @return Bytes read/written, the resulting seek offset, zero for successful
 * flush, or -1 when the underlying operation reported an error.
 */
extern int64_t bounce_file_io_result(const BOUNCE_FILE_IO *operation);

/**
 * @brief Get the file I/O operation error code.
 * @param operation Initialized file I/O operation storage.
 * @return POSIX errno value. Returns zero when the operation result is not an
 * error.
 */
extern int bounce_file_io_error(const BOUNCE_FILE_IO *operation);

/**
 * @brief Check whether a file I/O operation is currently active.
 * @param operation Initialized file I/O operation storage.
 * @return True when an operation is active.
 */
extern bool bounce_file_io_active(const BOUNCE_FILE_IO *operation);

/**
 * @brief Await a file read operation.
 * @param r Initialized BOUNCE_CORE.
 * @param operation Initialized file I/O operation storage.
 * @param fd File descriptor to read from.
 * @param buffer Destination buffer.
 * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
 * @param length Maximum bytes to read.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when local setup succeeded.
 * @remarks Linux uses io_uring when the POSIX core initialized it
 * successfully. Otherwise the POSIX fallback waits for fd readability and then
 * runs `read()` / `pread()` on the parked thread. For regular files and other
 * descriptors, that fallback syscall can still block while the operation is
 * executing.
 */
extern bool bounce_await_file_read(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  int fd,
  void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Await a file write operation.
 * @param r Initialized BOUNCE_CORE.
 * @param operation Initialized file I/O operation storage.
 * @param fd File descriptor to write to.
 * @param buffer Source buffer.
 * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
 * @param length Maximum bytes to write.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when local setup succeeded.
 * @remarks Linux uses io_uring when the POSIX core initialized it
 * successfully. Otherwise the POSIX fallback waits for fd writability and then
 * runs `write()` / `pwrite()` on the parked thread. For regular files and
 * other descriptors, that fallback syscall can still block while the operation
 * is executing.
 */
extern bool bounce_await_file_write(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  int fd,
  const void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Await a file seek operation.
 * @param r Initialized BOUNCE_CORE.
 * @param operation Initialized file I/O operation storage.
 * @param fd File descriptor to seek.
 * @param offset Offset passed to `lseek()`.
 * @param whence Seek base passed to `lseek()`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when local setup succeeded.
 * @remarks POSIX seek is queued onto the parked thread and then executes
 * `lseek()`. Cancellation can win before that queued operation starts.
 */
extern bool bounce_await_file_seek(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  int fd,
  int64_t offset,
  int whence,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

/**
 * @brief Await a file flush operation.
 * @param r Initialized BOUNCE_CORE.
 * @param operation Initialized file I/O operation storage.
 * @param fd File descriptor to flush.
 * @param mode Full metadata flush or data-only flush.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 * @return True when local setup succeeded.
 * @remarks Linux uses io_uring fsync when the POSIX core initialized it
 * successfully. Otherwise the POSIX fallback queues `fsync()` / `fdatasync()`
 * onto the parked thread. That fallback syscall can block while executing.
 */
extern bool bounce_await_file_flush(
  BOUNCE_CORE *r,
  BOUNCE_FILE_IO *operation,
  int fd,
  BOUNCE_FILE_FLUSH_MODE mode,
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
 * @brief Caller-owned file I/O operation storage for the C++ helper API.
 * @tparam TBOUNCE_CORE Backend bounce core storage type.
 * @tparam TBOUNCE_FILE_IO Backend file I/O operation storage type.
 */
template <typename TBOUNCE_CORE, typename TBOUNCE_FILE_IO> class file_io_base {
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
    ::bounce_file_io_init(&operation_);
  }

  inline ~file_io_base() noexcept {
    ::bounce_file_io_deinit(&operation_);
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
    return ::bounce_file_io_result(&operation_);
  }

  /**
   * @brief Get the last operation error code.
   * @return POSIX errno value, or zero when there was no syscall error.
   */
  inline int error() const noexcept {
    return ::bounce_file_io_error(&operation_);
  }

  /**
   * @brief Check whether this operation storage is active.
   * @return True when an operation is active.
   */
  inline bool active() const noexcept {
    return ::bounce_file_io_active(&operation_);
  }

  /**
   * @brief Await a file read operation through a bounce handle.
   * @param bounce_handle Bounce handle used to publish the completion.
   * @param fd File descriptor to read from.
   * @param buffer Destination buffer.
   * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
   * @param length Maximum bytes to read.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool read(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    void *buffer,
    int64_t offset,
    size_t length,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_file_read(
               core,
               &operation_,
               fd,
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
   * @param fd File descriptor to read from.
   * @param buffer Destination buffer.
   * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
   * @param length Maximum bytes to read.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   * @remarks Exceptions must not escape the callable.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool read(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
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
          fd,
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
   * @param fd File descriptor to write to.
   * @param buffer Source buffer.
   * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
   * @param length Maximum bytes to write.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool write(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    const void *buffer,
    int64_t offset,
    size_t length,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_file_write(
               core,
               &operation_,
               fd,
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
   * @param fd File descriptor to write to.
   * @param buffer Source buffer.
   * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
   * @param length Maximum bytes to write.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   * @remarks Exceptions must not escape the callable.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool write(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
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
          fd,
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
   * @param fd File descriptor to seek.
   * @param offset Offset passed to `lseek()`.
   * @param whence Seek base passed to `lseek()`.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool seek(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    int64_t offset,
    int whence,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_file_seek(
               core,
               &operation_,
               fd,
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
   * @param fd File descriptor to seek.
   * @param offset Offset passed to `lseek()`.
   * @param whence Seek base passed to `lseek()`.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   * @remarks Exceptions must not escape the callable.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool seek(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
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
          fd,
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
   * @param fd File descriptor to flush.
   * @param mode Full metadata flush or data-only flush.
   * @param completion Completion callback entry point.
   * @param completion_state User provided completion callback state.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   */
  template<typename TBOUNCE_HANDLE>
  inline bool flush(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
    BOUNCE_FILE_FLUSH_MODE mode,
    BOUNCE_COMPLETION completion,
    void *completion_state,
    BOUNCE_CANCELLATION *cancellation) noexcept {
    TBOUNCE_CORE *core = bounce_handle.get_core();

    return (core != nullptr) ?
             ::bounce_await_file_flush(
               core,
               &operation_,
               fd,
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
   * @param fd File descriptor to flush.
   * @param mode Full metadata flush or data-only flush.
   * @param completion Callable completion entry point.
   * @param cancellation Cancellation when provided.
   * @return True when local setup succeeded.
   * @remarks Exceptions must not escape the callable.
   */
  template<typename TBOUNCE_HANDLE, typename COMPLETION_TYPE>
  inline bool flush(
    TBOUNCE_HANDLE &bounce_handle,
    int fd,
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
          fd,
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

#if LIBBOUNCE_HAS_COROUTINE_SUPPORT
/**
 * @brief Await a file read operation inside a coroutine.
 * @param bounce_handle Bounce handle used to publish the completion.
 * @param fd File descriptor to read from.
 * @param buffer Destination buffer.
 * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
 * @param length Maximum bytes to read.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> read_async(
  bounce &bounce_handle,
  int fd,
  void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file read operation through a non-owning bounce reference.
 * @param bounce_handle Bounce reference used to publish the completion.
 * @param fd File descriptor to read from.
 * @param buffer Destination buffer.
 * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
 * @param length Maximum bytes to read.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> read_async(
  bounce_ref bounce_handle,
  int fd,
  void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file read operation through the current or fallback bounce.
 * @param fd File descriptor to read from.
 * @param buffer Destination buffer.
 * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
 * @param length Maximum bytes to read.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> read_async(
  int fd,
  void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file write operation inside a coroutine.
 * @param bounce_handle Bounce handle used to publish the completion.
 * @param fd File descriptor to write to.
 * @param buffer Source buffer.
 * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
 * @param length Maximum bytes to write.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> write_async(
  bounce &bounce_handle,
  int fd,
  const void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file write operation through a non-owning bounce reference.
 * @param bounce_handle Bounce reference used to publish the completion.
 * @param fd File descriptor to write to.
 * @param buffer Source buffer.
 * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
 * @param length Maximum bytes to write.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> write_async(
  bounce_ref bounce_handle,
  int fd,
  const void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file write operation through the current or fallback bounce.
 * @param fd File descriptor to write to.
 * @param buffer Source buffer.
 * @param offset Non-negative file offset, or `BOUNCE_FILE_OFFSET_CURRENT`.
 * @param length Maximum bytes to write.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> write_async(
  int fd,
  const void *buffer,
  int64_t offset,
  size_t length,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file seek operation inside a coroutine.
 * @param bounce_handle Bounce handle used to publish the completion.
 * @param fd File descriptor to seek.
 * @param offset Offset passed to `lseek()`.
 * @param whence Seek base passed to `lseek()`.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> seek_async(
  bounce &bounce_handle,
  int fd,
  int64_t offset,
  int whence,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file seek operation through a non-owning bounce reference.
 * @param bounce_handle Bounce reference used to publish the completion.
 * @param fd File descriptor to seek.
 * @param offset Offset passed to `lseek()`.
 * @param whence Seek base passed to `lseek()`.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> seek_async(
  bounce_ref bounce_handle,
  int fd,
  int64_t offset,
  int whence,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file seek operation through the current or fallback bounce.
 * @param fd File descriptor to seek.
 * @param offset Offset passed to `lseek()`.
 * @param whence Seek base passed to `lseek()`.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> seek_async(
  int fd,
  int64_t offset,
  int whence,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file flush operation inside a coroutine.
 * @param bounce_handle Bounce handle used to publish the completion.
 * @param fd File descriptor to flush.
 * @param mode Full metadata flush or data-only flush.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> flush_async(
  bounce &bounce_handle,
  int fd,
  BOUNCE_FILE_FLUSH_MODE mode = BOUNCE_FILE_FLUSH_FULL,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file flush operation through a non-owning bounce reference.
 * @param bounce_handle Bounce reference used to publish the completion.
 * @param fd File descriptor to flush.
 * @param mode Full metadata flush or data-only flush.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> flush_async(
  bounce_ref bounce_handle,
  int fd,
  BOUNCE_FILE_FLUSH_MODE mode = BOUNCE_FILE_FLUSH_FULL,
  BOUNCE_CANCELLATION *cancellation = nullptr);

/**
 * @brief Await a file flush operation through the current or fallback bounce.
 * @param fd File descriptor to flush.
 * @param mode Full metadata flush or data-only flush.
 * @param cancellation Cancellation when provided.
 * @return Promise resolving to the file I/O result.
 */
promise<file_io_result> flush_async(
  int fd,
  BOUNCE_FILE_FLUSH_MODE mode = BOUNCE_FILE_FLUSH_FULL,
  BOUNCE_CANCELLATION *cancellation = nullptr);
#endif

}
#endif

#endif

#endif

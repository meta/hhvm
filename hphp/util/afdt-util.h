/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#pragma once

#include <sys/socket.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <type_traits>

namespace HPHP {
namespace afdt {
///////////////////////////////////////////////////////////////////////////////

namespace detail {

void sappend(std::vector<iovec>& iov, const void* addr, size_t size);
void rappend(std::vector<iovec>& iov, void* addr, size_t size);

template<class T>
typename std::enable_if<
  std::is_fundamental<T>::value || std::is_pod<T>::value,
  void
>::type sappend(std::vector<iovec>& iov, const T& elm) {
  sappend(iov, &elm, sizeof elm);
}

template<class T>
typename std::enable_if<
  std::is_fundamental<T>::value || std::is_pod<T>::value,
  void
>::type rappend(std::vector<iovec>& iov, T* elm) {
  rappend(iov, elm, sizeof *elm);
}

void send(int afdt_fd, std::vector<iovec>& iov);

template<class... Tail>
void send(int afdt_fd, std::vector<iovec>& iov,
          const std::vector<std::string>& h, Tail&&... args);

template<class... Tail>
void send(int afdt_fd, std::vector<iovec>& iov,
          const std::string& h, Tail&&... args);

template<class... Tail>
void send(int afdt_fd, std::vector<iovec>& iov,
          const char* h, Tail&&... args);

template<class T, class... Tail>
typename std::enable_if<std::is_fundamental<T>::value, void>::type
send(int afdt_fd, std::vector<iovec>& iov,
     const std::vector<T>& h, Tail&&... args);

template<class... Tail>
void send(int afdt_fd, std::vector<iovec>& iov,
          const std::vector<std::string>& h, Tail&&... args);


template<class Head, class... Tail>
void send(int afdt_fd, std::vector<iovec>& iov,
          const Head& h, Tail&&... args) {
  sappend(iov, h);
  send(afdt_fd, iov, std::forward<Tail>(args)...);
}

template<class... Tail>
void send(int afdt_fd, std::vector<iovec>& iov,
          const std::string& h, Tail&&... args) {
  size_t s = h.size();
  sappend(iov, s);
  sappend(iov, &h[0], s);
  send(afdt_fd, iov, std::forward<Tail>(args)...);
}

template<class... Tail>
void send(int afdt_fd, std::vector<iovec>& iov,
          const char* h, Tail&&... args) {
  size_t s = std::strlen(h);
  sappend(iov, s);
  sappend(iov, &h[0], s);
  send(afdt_fd, iov, std::forward<Tail>(args)...);
}

template<class T, class... Tail>
typename std::enable_if<std::is_fundamental<T>::value, void>::type
send(int afdt_fd, std::vector<iovec>& iov,
     const std::vector<T>& h, Tail&&... args) {
  size_t s = h.size();
  sappend(iov, s);
  sappend(iov, &h[0], s * sizeof(T));
  send(afdt_fd, iov, std::forward<Tail>(args)...);
}

template<class... Tail>
void send(int afdt_fd, std::vector<iovec>& iov,
          const std::vector<std::string>& h, Tail&&... args) {
  size_t s = h.size();
  sappend(iov, s);
  std::vector<size_t> sizes;
  sizes.reserve(s);
  for (auto& e : h) {
    auto strsz = e.size();
    sizes.push_back(strsz);
    sappend(iov, sizes.back());
    sappend(iov, &e[0], strsz);
  }
  send(afdt_fd, iov, std::forward<Tail>(args)...);
}

void recv(int afdt_fd, std::vector<iovec>& iov);

template<class... Tail>
void recv(int afdt_fd, std::vector<iovec>& iov,
          std::string& h, Tail&... args);

template<class T, class... Tail>
typename std::enable_if<std::is_fundamental<T>::value, void>::type
recv(int afdt_fd, std::vector<iovec>& iov,
     std::vector<T>& h, Tail&... args);

template<class... Tail>
void recv(int afdt_fd, std::vector<iovec>& iov,
          std::vector<std::string>& h, Tail&... args);


template<class Head, class... Tail>
void recv(int afdt_fd, std::vector<iovec>& iov,
          Head& h, Tail&... args) {
  rappend(iov, &h);
  recv(afdt_fd, iov, args...);
}

template<class... Tail>
void recv(int afdt_fd, std::vector<iovec>& iov,
          std::string& h, Tail&... args) {
  size_t sz;
  rappend(iov, &sz);
  recv(afdt_fd, iov);
  iov.clear();
  h.resize(sz);
  rappend(iov, &h[0], sz);
  recv(afdt_fd, iov, args...);
}

template<class T, class... Tail>
typename std::enable_if<std::is_fundamental<T>::value, void>::type
recv(int afdt_fd, std::vector<iovec>& iov,
     std::vector<T>& h, Tail&... args) {
  size_t sz;
  rappend(iov, &sz);
  recv(afdt_fd, iov);
  iov.clear();
  h.resize(sz);
  rappend(iov, &h[0], sz * sizeof(h[0]));
  recv(afdt_fd, iov, args...);
}

template<class... Tail>
void recv(int afdt_fd, std::vector<iovec>& iov,
          std::vector<std::string>& h, Tail&... args) {
  size_t sz;
  rappend(iov, &sz);
  recv(afdt_fd, iov);
  iov.clear();
  h.resize(sz);
  for (auto& e : h) {
    size_t strsz;
    rappend(iov, &strsz);
    recv(afdt_fd, iov);
    iov.clear();
    e.resize(strsz);
    rappend(iov, &e[0], strsz);
  }
  recv(afdt_fd, iov, args...);
}

}

template<class Head, class... Tail>
void sendx(int afdt_fd, const Head& h, Tail&&... args) {
  std::vector<iovec> iov;
  detail::send(afdt_fd, iov, h, std::forward<Tail>(args)...);
}

template<class Head, class... Tail>
void recvx(int afdt_fd, Head& h, Tail&... args) {
  std::vector<iovec> iov;
  detail::recv(afdt_fd, iov, h, args...);
}

bool send_fd(int afdt_fd, int fd);
int recv_fd(int afdt_fd);

///////////////////////////////////////////////////////////////////////////////
}
}

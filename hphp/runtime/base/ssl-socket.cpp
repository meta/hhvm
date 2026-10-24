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

#include "hphp/runtime/base/ssl-socket.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/req-ptr.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/server/server-stats.h"

#include <folly/String.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/SysTime.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

bool SSLSocketData::closeImpl() {
  if (m_ssl_active) {
    SSL_shutdown(m_handle);
    m_ssl_active = false;
  }
  if (m_handle) {
    SSL_free(m_handle);
    SSL_CTX_free(m_ctx);
    m_handle = nullptr;
    m_ctx = nullptr;
  }
  return SocketData::closeImpl();
}

SSLSocketData::~SSLSocketData() {
  SSLSocketData::closeImpl();
}

Mutex SSLSocket::s_mutex;
int SSLSocket::s_ex_data_index = -1;
int SSLSocket::GetSSLExDataIndex() {
  if (s_ex_data_index >= 0) {
    return s_ex_data_index;
  }
  Lock lock(s_mutex);
  if (s_ex_data_index < 0) {
    s_ex_data_index = SSL_get_ex_new_index(0, (void*)"PHP stream index",
                                           nullptr, nullptr, nullptr);
    assertx(s_ex_data_index >= 0);
  }
  return s_ex_data_index;
}

const StaticString
  s_allow_self_signed("allow_self_signed"),
  s_verify_depth("verify_depth");

int SSLSocket::verifyCallback(int preverify_ok, X509_STORE_CTX *ctx) {
  int ret = preverify_ok;

  /* determine the status for the current cert */
  X509_STORE_CTX_get_current_cert(ctx);
  int err = X509_STORE_CTX_get_error(ctx);
  int depth = X509_STORE_CTX_get_error_depth(ctx);

  /* conjure the stream & context to use */
  SSL *ssl = (SSL*)X509_STORE_CTX_get_ex_data
    (ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
  SSLSocket *stream =
    (SSLSocket*)SSL_get_ex_data(ssl, SSLSocket::GetSSLExDataIndex());

  /* if allow_self_signed is set, make sure that verification succeeds */
  if (err == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT &&
      stream->m_context[s_allow_self_signed].toBoolean()) {
    ret = 1;
  }

  /* check the depth */
  Variant vdepth = stream->m_context[s_verify_depth];
  if (vdepth.toBoolean() && depth > vdepth.toInt64()) {
    ret = 0;
    X509_STORE_CTX_set_error(ctx, X509_V_ERR_CERT_CHAIN_TOO_LONG);
  }

  return ret;
}

const StaticString s_passphrase("passphrase");

int SSLSocket::passwdCallback(char* buf, int num, int /*verify*/, void* data) {
  /* TODO: could expand this to make a callback into PHP user-space */
  SSLSocket *stream = (SSLSocket *)data;
  OptString passphrase = stream->m_context[s_passphrase].toString();
  if (!passphrase.empty() && passphrase.size() < num - 1) {
    memcpy(buf, passphrase.data(), passphrase.size() + 1);
    return passphrase.size();
  }
  return 0;
}

const StaticString
  s_verify_peer("verify_peer"),
  s_cafile("cafile"),
  s_capath("capath"),
  s_ciphers("ciphers"),
  s_local_cert("local_cert");

SSL *SSLSocket::createSSL(SSL_CTX *ctx) {
  ERR_clear_error();

  /* look at options in the stream and set appropriate verification flags */
  if (m_context[s_verify_peer].toBoolean()) {
    /* turn on verification callback */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verifyCallback);

    /* CA stuff */
    OptString cafile = m_context[s_cafile].toString();
    OptString capath = m_context[s_capath].toString();

    if (!FileUtil::isValidPath(cafile)) {
      raise_warning("cafile expected to be a path, string given");
      return nullptr;
    } else if (!FileUtil::isValidPath(capath)) {
      raise_warning("capath expected to be a path, string given");
      return nullptr;
    } else if (!cafile.empty() || !capath.empty()) {
      const char* cafileptr = cafile.empty() ? nullptr : cafile.data();
      const char* capathptr = capath.empty() ? nullptr : capath.data();
      if (!SSL_CTX_load_verify_locations(ctx, cafileptr, capathptr)) {
        raise_warning("Unable to set verify locations `%s' `%s'",
                      cafile.data(), capath.data());
        return nullptr;
      }
    } else if (m_data->m_client && !SSL_CTX_set_default_verify_paths(ctx)) {
      raise_warning(
        "Unable to set default verify locations and no CA settings specified");
      return nullptr;
    }

    int64_t depth = m_context[s_verify_depth].toInt64();
    if (depth) {
      SSL_CTX_set_verify_depth(ctx, depth);
    }
  } else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  }

  /* callback for the passphrase (for localcert) */
  if (!m_context[s_passphrase].toString().empty()) {
    SSL_CTX_set_default_passwd_cb_userdata(ctx, this);
    SSL_CTX_set_default_passwd_cb(ctx, passwdCallback);
  }

  OptString cipherlist = m_context[s_ciphers].toString();
  if (cipherlist.empty()) {
    cipherlist = "DEFAULT";
  }
  SSL_CTX_set_cipher_list(ctx, cipherlist.data());

  OptString certfile = m_context[s_local_cert].toString();
  if (!FileUtil::isValidPath(certfile)) {
    raise_warning("local_cert expected to be a path, string given");
    return nullptr;
  } else if (!certfile.empty()) {
    OptString resolved_path_buff = File::TranslatePath(certfile);
    if (!resolved_path_buff.empty()) {
      /* a certificate to use for authentication */
      if (SSL_CTX_use_certificate_chain_file(ctx, resolved_path_buff.data())
          != 1) {
        raise_warning("Unable to set local cert chain file `%s'; Check "
                      "that your cafile/capath settings include details of "
                      "your certificate and its issuer", certfile.data());
        return nullptr;
      }

      if (SSL_CTX_use_PrivateKey_file(ctx, resolved_path_buff.data(),
                                      SSL_FILETYPE_PEM) != 1) {
        raise_warning("Unable to set private key file `%s'",
                      resolved_path_buff.data());
        return nullptr;
      }

      SSL *tmpssl = SSL_new(ctx);
      X509 *cert = SSL_get_certificate(tmpssl);
      if (cert) {
        EVP_PKEY *key = X509_get_pubkey(cert);
        EVP_PKEY_copy_parameters(key, SSL_get_privatekey(tmpssl));
        EVP_PKEY_free(key);
      }
      SSL_free(tmpssl);

      if (!SSL_CTX_check_private_key(ctx)) {
        raise_warning("Private key does not match certificate!");
      }
    }
  }

  SSL *ssl = SSL_new(ctx);
  if (ssl) {
    SSL_set_ex_data(ssl, GetSSLExDataIndex(), this); /* map SSL => stream */
  }
  return ssl;
}

///////////////////////////////////////////////////////////////////////////////
// constructors and destructor

SSLSocket::SSLSocket(bool nonblocking /* = true*/)
: Socket(std::make_shared<SSLSocketData>(nonblocking)),
  m_data(static_cast<SSLSocketData*>(getSocketData()))
{}

SSLSocket::SSLSocket(std::shared_ptr<SSLSocketData> data)
: Socket(data),
  m_data(static_cast<SSLSocketData*>(getSocketData()))
{}

StaticString s_ssl("ssl");

SSLSocket::SSLSocket(int sockfd, int type, const req::ptr<StreamContext>& ctx,
                     const char *address /* = NULL */, int port /* = 0 */,
                     bool nonblocking /* = true*/)
: Socket(std::make_shared<SSLSocketData>(port, type, nonblocking),
        sockfd, type, address, port),
  m_data(static_cast<SSLSocketData*>(getSocketData()))
{
  if (!ctx) {
    return;
  }
  this->setStreamContext(ctx);
  this->m_context = ctx->getOptions()[s_ssl].toArray();
}

SSLSocket::~SSLSocket() { }

void SSLSocket::sweep() {
  File::close();
  File::sweep();
  m_data = nullptr;
}

bool SSLSocket::enableCrypto(CryptoMethod method) {
  if (this->m_data->m_method != CryptoMethod::NoCrypto) {
    raise_warning("SSL/TLS already set-up for this stream");
    return false;
  }
  this->m_data->m_method = method;
  return setupCrypto() && enableCrypto();
}

bool SSLSocket::disableCrypto() {
  if (!this->m_data->m_ssl_active) {
    return false;
  }
  this->m_data->m_method = CryptoMethod::NoCrypto;
  SSL_shutdown(this->m_data->m_handle);
  this->m_data->m_ssl_active = false;
  return true;
}

bool SSLSocket::onConnect() {
  if (this->m_data->m_method == CryptoMethod::NoCrypto) {
    return true;
  }
  return setupCrypto() && enableCrypto();
}

bool SSLSocket::onAccept() {
  if (m_data->m_method == CryptoMethod::NoCrypto) {
    return true;
  }
  if (getFd() >= 0 && m_data->m_enable_on_connect) {
    switch (m_data->m_method) {
    case CryptoMethod::ClientSSLv23:
      m_data->m_method = CryptoMethod::ServerSSLv23;
      break;
    case CryptoMethod::ClientSSLv2:
      m_data->m_method = CryptoMethod::ServerSSLv2;
      break;
    case CryptoMethod::ClientSSLv3:
      m_data->m_method = CryptoMethod::ServerSSLv3;
      break;
    case CryptoMethod::ClientTLS:
      m_data->m_method = CryptoMethod::ServerTLS;
      break;
    default:
      assertx(false);
    }

    if (setupCrypto() && enableCrypto()) {
      return true;
    }

    raise_warning("Failed to enable crypto");
    close();
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////

bool SSLSocket::handleError(int64_t nr_bytes, bool is_init) {
  char esbuf[512];
  std::string ebuf;
  unsigned long ecode;

  bool retry = true;
  int err = SSL_get_error(m_data->m_handle, nr_bytes);
  switch (err) {
  case SSL_ERROR_ZERO_RETURN:
    /* SSL terminated (but socket may still be active) */
    retry = false;
    break;
  case SSL_ERROR_WANT_READ:
  case SSL_ERROR_WANT_WRITE:
    /* re-negotiation, or perhaps the SSL layer needs more
     * packets: retry in next iteration */
    errno = EAGAIN;
    retry = (is_init || m_data->m_is_blocked);
    break;
  case SSL_ERROR_SYSCALL:
    if (ERR_peek_error() == 0) {
      if (nr_bytes == 0) {
        if (ERR_get_error()) {
          raise_warning("SSL: fatal protocol error");
        }
        SSL_set_shutdown(m_data->m_handle,
                         SSL_SENT_SHUTDOWN|SSL_RECEIVED_SHUTDOWN);
        setEof(true);
        retry = false;
      } else {
        raise_warning("SSL: %s", folly::errnoStr(errno).c_str());
        retry = false;
      }
      break;
    }
    [[fallthrough]];
  default:
    /* some other error */
    ecode = ERR_get_error();
    switch (ERR_GET_REASON(ecode)) {
    case SSL_R_NO_SHARED_CIPHER:
      raise_warning("SSL_R_NO_SHARED_CIPHER: no suitable shared cipher "
                    "could be used.  This could be because the server is "
                    "missing an SSL certificate (local_cert context "
                    "option)");
      retry = false;
      break;

    default:
      do {
        // NULL is automatically added
        ERR_error_string_n(ecode, esbuf, sizeof(esbuf));
        if (!ebuf.empty()) {
          ebuf += '\n';
        }
        ebuf += esbuf;
      } while ((ecode = ERR_get_error()) != 0);

      raise_warning("SSL operation failed with code %d. %s%s",
                    err, !ebuf.empty() ? "OpenSSL Error messages:\n" : "",
                    !ebuf.empty() ? ebuf.c_str() : "");
    }

    retry = false;
    errno = 0;
  }

  return retry;
}

///////////////////////////////////////////////////////////////////////////////

req::ptr<SSLSocket> SSLSocket::Create(
  int fd, int domain, const HostURL &hosturl, double timeout,
  const req::ptr<StreamContext>& ctx,
  bool nonblocking
) {
  CryptoMethod method;
  const std::string scheme = hosturl.getScheme();

  if (scheme == "ssl") {
    method = CryptoMethod::ClientSSLv23;
  } else if (scheme == "sslv2") {
    method = CryptoMethod::ClientSSLv2;
  } else if (scheme == "sslv3") {
    method = CryptoMethod::ClientSSLv3;
  } else if (scheme == "tls") {
    method = CryptoMethod::ClientTLS;
  } else if (
    scheme == "tcp"
    || (scheme.empty() && (domain == AF_INET || domain == AF_INET6))
  ) {
    method = CryptoMethod::NoCrypto;
  } else {
    return nullptr;
  }

  return Create(fd, domain, method, hosturl.getHost(), hosturl.getPort(),
                timeout, ctx, nonblocking);
}

req::ptr<SSLSocket> SSLSocket::Create(
  int fd, int domain, CryptoMethod method, std::string address, int port,
  double timeout, const req::ptr<StreamContext>& ctx,
  bool nonblocking
) {
  auto sock = req::make<SSLSocket>(
    fd, domain, ctx, address.c_str(), port, nonblocking);

  sock->m_data->m_method = method;
  sock->m_data->m_connect_timeout = timeout;
  sock->m_data->m_enable_on_connect = true;

  return sock;
}

bool SSLSocket::waitForData() {
  if (m_data->m_ssl_active && SSL_pending(m_data->m_handle)) {
    return true;
  }

  return Socket::waitForData();
}

int64_t SSLSocket::readImpl(char *buffer, int64_t length) {
  int64_t nr_bytes = 0;
  if (m_data->m_ssl_active) {
    IOStatusHelper io("sslsocket::recv", getAddress().c_str(), getPort());

    bool retry = true;
    do {
      if (m_data->m_is_blocked) {
        waitForData();
        if (timedOut()) {
          break;
        }
        // could get here and we only have parts of an SSL packet
      }
      nr_bytes = SSL_read(m_data->m_handle, buffer, length);
      if (nr_bytes > 0) break; /* we got the data */
      retry = handleError(nr_bytes, false);
      setEof(!retry && errno != EAGAIN && !SSL_pending(m_data->m_handle));
    } while (retry);
  } else {
    nr_bytes = Socket::readImpl(buffer, length);
  }
  return nr_bytes < 0 ? 0 : nr_bytes;
}

int64_t SSLSocket::writeImpl(const char *buffer, int64_t length) {
  int didwrite;
  if (m_data->m_ssl_active) {
    IOStatusHelper io("sslsocket::send", getAddress().c_str(), getPort());

    bool retry = true;
    do {
      didwrite = SSL_write(m_data->m_handle, buffer, length);
      if (didwrite > 0) break;
      retry = handleError(didwrite, false);
    } while (retry);
  } else {
    didwrite = Socket::writeImpl(buffer, length);
  }
  return didwrite < 0 ? 0 : didwrite;
}

bool SSLSocket::setupCrypto(SSLSocket *session /* = NULL */) {
  if (m_data->m_handle) {
    raise_warning("SSL/TLS already set-up for this stream");
    return false;
  }

  if (!m_context.exists(s_verify_peer)) {
    m_context.set(s_verify_peer, true);
  }

  /* need to do slightly different things, based on client/server method,
   * so lets remember which method was selected */
#if OPENSSL_VERSION_NUMBER < 0x00909000L
  SSL_METHOD *smethod;
#else
  const SSL_METHOD *smethod;
#endif
  switch (m_data->m_method) {
  case CryptoMethod::ClientSSLv23:
    m_data->m_client = true;
    smethod = SSLv23_client_method();
    break;
  case CryptoMethod::ClientTLS:
    m_data->m_client = true;
    smethod = TLS_client_method();
    break;
  case CryptoMethod::ServerSSLv23:
    m_data->m_client = false;
    smethod = SSLv23_server_method();
    break;

#ifndef OPENSSL_NO_SSL3
  case CryptoMethod::ClientSSLv3:
    m_data->m_client = true;
    smethod = SSLv3_client_method();
    break;
  case CryptoMethod::ServerSSLv3:
    m_data->m_client = false;
    smethod = SSLv3_server_method();
    break;
#else
  case CryptoMethod::ClientSSLv3:
  case CryptoMethod::ServerSSLv3:
    raise_warning("OpenSSL library does not support SSL3 protocol");
    return false;
#endif

  case CryptoMethod::ClientSSLv2:
  case CryptoMethod::ServerSSLv2:
    raise_warning("OpenSSL library does not support SSL2 protocol");
    return false;

  case CryptoMethod::ServerTLS:
    m_data->m_client = false;
    smethod = TLS_server_method();
    break;
  default:
    return false;
  }

  SSL_CTX *ctx = SSL_CTX_new(smethod);
  if (ctx == nullptr) {
    raise_warning("failed to create an SSL context");
    return false;
  }

  SSL_CTX_set_options(ctx, SSL_OP_ALL);
  m_data->m_handle = createSSL(ctx);
  if (m_data->m_handle == nullptr) {
    raise_warning("failed to create an SSL handle");
    SSL_CTX_free(ctx);
    return false;
  }

  m_data->m_ctx = ctx;

  if (!SSL_set_fd(m_data->m_handle, getFd())) {
    handleError(0, true);
  }
  if (session) {
    SSL_copy_session_id(m_data->m_handle, session->m_data->m_handle);
  }
  return true;
}

const StaticString s_CN_match("CN_match");

bool SSLSocket::applyVerificationPolicy(X509 *peer) {
  /* verification is turned off */
  if (!m_context[s_verify_peer].toBoolean()) {
    return true;
  }

  if (peer == nullptr) {
    raise_warning("Could not get peer certificate");
    return false;
  }

  int err = SSL_get_verify_result(m_data->m_handle);
  switch (err) {
  case X509_V_OK:
    /* fine */
    break;
  case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
    if (m_context[s_allow_self_signed].toBoolean()) {
      /* allowed */
      break;
    }
    /* not allowed, so fall through */
    [[fallthrough]];
  default:
    raise_warning("Could not verify peer: code:%d %s", err,
                  X509_verify_cert_error_string(err));
    return false;
  }

  /* if the cert passed the usual checks, apply our own local policies now */

  /* Does the common name match ? (used primarily for https://) */
  OptString cnmatch = m_context[s_CN_match].toString();
  if (!cnmatch.empty()) {
    X509_NAME *name = X509_get_subject_name(peer);
    char buf[1024];
    int name_len = X509_NAME_get_text_by_NID(name, NID_commonName, buf,
                                             sizeof(buf));

    if (name_len < 0) {
      raise_warning("Unable to locate peer certificate CN");
      return false;
    } else if (name_len != (int)strlen(buf)) {
      raise_warning("Peer certificate CN=`%.*s' is malformed", name_len, buf);
      return false;
    } else if (name_len == sizeof(buf)) {
      raise_warning("Peer certificate CN=`%.*s' is too long to verify",
                    name_len, buf);
      return false;
    }

    bool match = (strcmp(cnmatch.c_str(), buf) == 0);
    if (!match && strlen(buf) > 3 && buf[0] == '*' && buf[1] == '.') {
      /* Try wildcard */
      if (strchr(buf+2, '.')) {
        const char* cnmatch_str = cnmatch.c_str();
        const char *tmp = strstr(cnmatch_str, buf+1);
        match = tmp && strcmp(tmp, buf+2) && tmp == strchr(cnmatch_str, '.');
      }
    }

    if (!match) {
      /* didn't match */
      raise_warning("Peer certificate CN=`%.*s' did not match expected CN=`%s'",
                    name_len, buf, cnmatch.c_str());
      return false;
    }
  }

  return true;
}

const StaticString
  s_capture_peer_cert("capture_peer_cert"),
  s_peer_certificate("peer_certificate"),
  s_capture_peer_cert_chain("capture_peer_cert_chain"),
  s_peer_certificate_chain("peer_certificate_chain");

bool SSLSocket::enableCrypto(bool activate /* = true */) {
  if (activate && !m_data->m_ssl_active) {
    double timeout = m_data->m_connect_timeout;
    bool blocked = m_data->m_is_blocked;
    if (!m_data->m_state_set) {
      if (m_data->m_client) {
        SSL_set_connect_state(m_data->m_handle);
      } else {
        SSL_set_accept_state(m_data->m_handle);
      }
      m_data->m_state_set = true;
    }

    if (m_data->m_client && setBlocking(false)) {
      m_data->m_is_blocked = false;
    }

    int n;
    bool retry = true;
    do {
      if (m_data->m_client) {
        struct timeval tvs, tve;
        struct timezone tz;

        gettimeofday(&tvs, &tz);
        n = SSL_connect(m_data->m_handle);
        gettimeofday(&tve, &tz);

        timeout -= (tve.tv_sec + (double) tve.tv_usec / 1000000) -
          (tvs.tv_sec + (double) tvs.tv_usec / 1000000);
        if (timeout < 0) {
          raise_warning("SSL: connection timeout");
          return false;
        }
      } else {
        n = SSL_accept(m_data->m_handle);
      }

      if (n <= 0) {
        retry = handleError(n, true);
      } else {
        break;
      }
    } while (retry);

    if (m_data->m_client &&
        m_data->m_is_blocked != blocked &&
        setBlocking(blocked)) {
      m_data->m_is_blocked = blocked;
    }

    if (n == 1) {
      X509 *peer_cert = SSL_get_peer_certificate(m_data->m_handle);
      if (!applyVerificationPolicy(peer_cert)) {
        SSL_shutdown(m_data->m_handle);
      } else {
        m_data->m_ssl_active = true;

        /* allow the script to capture the peer cert
         * and/or the certificate chain */
        if (m_context[s_capture_peer_cert].toBoolean()) {
          m_context.set(s_peer_certificate,
                        Variant(req::make<Certificate>(peer_cert)));
          peer_cert = nullptr;
        }

        if (m_context[s_capture_peer_cert_chain].toBoolean()) {
          Array arr;
          STACK_OF(X509) *chain = SSL_get_peer_cert_chain(m_data->m_handle);
          if (chain) {
            for (int i = 0; i < sk_X509_num(chain); i++) {
              X509 *mycert = X509_dup(sk_X509_value(chain, i));
              arr.append(Variant(req::make<Certificate>(mycert)));
            }
          }
          m_context.set(s_peer_certificate_chain, arr);
        }
      }

      if (peer_cert) {
        X509_free(peer_cert);
      }
    } else  {
      n = errno == EAGAIN ? 0 : -1;
    }

    return n >= 0;

  } else if (!activate && m_data->m_ssl_active) {
    /* deactivate - common for server/client */
    SSL_shutdown(m_data->m_handle);
    m_data->m_ssl_active = false;
  }
  return true;
}

bool SSLSocket::checkLiveness() {
  if (getFd() == -1) {
    return false;
  }

  pollfd p;
  p.fd = getFd();
  p.events = POLLIN | POLLERR | POLLHUP | POLLPRI;
  p.revents = 0;
  if (poll(&p, 1, 0) > 0 && p.revents > 0) {
    char buf;
    if (m_data->m_ssl_active) {
      while (true) {
        int n = SSL_peek(m_data->m_handle, &buf, sizeof(buf));
        if (n <= 0) {
          int err = SSL_get_error(m_data->m_handle, n);
          if (err == SSL_ERROR_SYSCALL) {
            return errno == EAGAIN;
          }

          if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            /* re-negotiate */
            continue;
          }

          /* any other problem is a fatal error */
          return false;
        }
        /* either peek succeeded or there was an error; we
         * have set the alive flag appropriately */
        break;
      }
    } else if (0 == recv(getFd(), &buf, sizeof(buf), MSG_PEEK) &&
               errno != EAGAIN) {
      return false;
    }
  }
  return true;
}

SSLSocket::CryptoMethod SSLSocket::getCryptoMethod() {
  return m_data->m_method;
}

///////////////////////////////////////////////////////////////////////////////
// Certificate

IMPLEMENT_RESOURCE_ALLOCATION(Certificate)

BIO *Certificate::ReadData(const Variant& var, bool *file /* = NULL */) {
  if (var.isString() || var.isObject()) {
    OptString svar = var.toString();
    if (StringUtil::IsFileUrl(svar)) {
      OptString decoded = StringUtil::DecodeFileUrl(svar);
      if (decoded.empty()) {
        raise_warning("invalid file URL, %s", svar.data());
        return nullptr;
      }
      if (file) *file = true;
      BIO *ret = BIO_new_file(decoded.data(), "r");
      if (ret == nullptr) {
        raise_warning("error opening the file, %s", decoded.data());
      }
      return ret;
    }

    if (file) *file = false;
    return BIO_new_mem_buf((char*)svar.data(), svar.size());
  }
  return nullptr;
}


req::ptr<Certificate> Certificate::Get(const Variant& var) {
  if (var.isResource()) {
    return dyn_cast_or_null<Certificate>(var);
  }
  if (var.isString() || var.isObject()) {
    bool file;
    BIO *in = ReadData(var, &file);
    if (in == nullptr) return nullptr;

    X509 *cert;
    /*
    if (file) {
      cert = PEM_read_bio_X509(in, NULL, NULL, NULL);
    } else {
      cert = (X509 *)PEM_ASN1_read_bio
        ((char *(*)())d2i_X509, PEM_STRING_X509, in, NULL, NULL, NULL);
    }
    */
    cert = PEM_read_bio_X509(in, nullptr, nullptr, nullptr);
    BIO_free(in);
    if (cert) {
      return req::make<Certificate>(cert);
    }
  }
  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////
}

#include "http.h"

namespace http {
void SessionBase::run() {
  asio::dispatch(stream_.get_executor(),
      std::bind_front(&SessionBase::read, shared_from_this()));
}

void SessionBase::read() {
  beast::http::async_read(stream_, buffer_, request_,
      std::bind_front(&SessionBase::on_read, shared_from_this()));
}

void SessionBase::on_read(beast::error_code error_code, std::size_t bytes_transferred) {
static const bool keep_alive = true;
  if (beast::http::error::end_of_stream == error_code) {
    close();
  }
  on_request();
}

void SessionBase::on_write(bool keep_alive, beast::error_code error_code, std::size_t bytes_transferred) {
  if (error_code) { }
  close();
}

void SessionBase::close() {
  beast::error_code error_code;
  stream_.socket().shutdown(tcp::socket::shutdown_send, error_code);
}

Listener * Listener::listen(tcp::endpoint endpoint) {
  beast::error_code error_code;

  acceptor_.open(endpoint.protocol(), error_code);
  if (error_code) { }

  acceptor_.set_option(asio::socket_base::reuse_address(true), error_code);
  if (error_code) { }

  acceptor_.bind(endpoint, error_code);
  if (error_code) { }

  acceptor_.listen(/* number of simultaneous connections */ 16, error_code);
  if (error_code) { }

  return this;
}

void Listener::close() {
  acceptor_.close();
}
} // end of http namespace

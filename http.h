#ifndef HTTP_H
#define HTTP_H

#include <memory>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

namespace http {

namespace beast = boost::beast;
namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

struct SessionBase : public std::enable_shared_from_this<SessionBase> {
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  beast::http::request<beast::http::string_body> request_;

  virtual ~SessionBase() = default;
  SessionBase(tcp::socket && socket) :
    stream_(std::move(socket)) { }
  void run();
  void read();
  virtual void on_request() = 0;
  void on_read(beast::error_code, std::size_t);
  void on_write(bool, beast::error_code, std::size_t);
  void close();
};

struct Listener : public std::enable_shared_from_this<Listener> {
  asio::io_context & io_context_;
  tcp::acceptor acceptor_;

  Listener(asio::io_context & io_context) :
    io_context_(io_context),
    acceptor_(asio::make_strand(io_context_)) { }

  Listener * listen(tcp::endpoint endpoint);

  template<class Session, class Tuple>
  void accept(Tuple && tuple) {
    acceptor_.async_accept(asio::make_strand(io_context_),
	std::bind_front(&Listener::on_accept<Session, Tuple>, shared_from_this(), std::move(tuple)));
  }

  template<class Session, class Tuple>
  void on_accept(Tuple && tuple, beast::error_code error_code, tcp::socket socket) {
    if (error_code) { return; }
    auto session = std::make_shared<Session>(std::move(socket));
    session->initialize(tuple);
    session->run();
    accept<Session>(std::move(tuple));
  }

  void close();
};
}

#endif //HTTP_H

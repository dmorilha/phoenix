#include <memory>
#include <sstream>
#include <thread>
#include <vector>
#include <string>

#include <cassert>

#include <boost/asio/signal_set.hpp>

#include "database.h"
#include "http.h"

using DatabasePool = std::shared_ptr<database::Pool>;

struct MySession : public http::SessionBase {
  std::shared_ptr<database::Pool> pool_;

  template <class ... Args>
    MySession(Args && ... args) : SessionBase(std::forward<Args>(args)...) { }

  void initialize(const std::tuple<DatabasePool> & input) {
    pool_ = std::move(std::get<0>(input));
  }

  template <class Body, class Allocator>
  std::string serializeRequest(const boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>> & request) {
    std::stringstream buffer;
    buffer << request.method() << " " << request.target() << " " << request.version() << " ; ";
    for (const auto & item : request) {
      buffer << item.name() << ": " << item.value() << " ; ";
    }
    return buffer.str();
  }

  template <class Body, class Allocator>
    boost::beast::http::message_generator
    handle_request(boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>> && request) {
      assert(nullptr != pool_);

      auto connection = pool_->getConnection().get();

      connection.change("INSERT INTO Request (request) VALUES (:request);",
          database::ParameterList{database::makeParameter(":request", serializeRequest(request))});

      auto table = connection.read("SELECT * FROM Request;", database::ParameterList{}, [](database::Cursor && cursor) {
          std::stringstream buffer;

          const int columns = cursor.columns();

          for (int i = 0; i < columns; ++i) {
            std::string columnName = cursor.name(i);
            columnName += " (";
            columnName += cursor.type(i);
            columnName += ')';
            columnName += " | ";
            if (i + 1 < columns)
              columnName += ' ';
            buffer << columnName.c_str();
          }

          const size_t header_size = buffer.str().size();
          buffer << std::endl << std::string(header_size, '-') << std::endl;

          while (cursor.step()) {
            for (int i = 0; i < columns; ++i)
              buffer << cursor.text(i);
            buffer << std::endl;
          }

          return buffer.str();
      });

      boost::beast::error_code error_code;
      boost::beast::http::file_body::value_type body;

      boost::beast::http::response<boost::beast::http::string_body> response(
          boost::beast::http::status::ok, request.version());

      response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
      response.set(boost::beast::http::field::content_type, "text/plain");
      response.keep_alive(request.keep_alive());
      std::string content = "(empty)";
      if (table) {
        content = *table;
      }
      response.content_length(content.size());
      response.body() = content;
      response.prepare_payload();

      return response;
    }

  void on_request() {
    boost::beast::async_write(stream_,
        handle_request(std::move(request_)),
        std::bind_front(&SessionBase::on_write, shared_from_this(), false));
  }
};


int main(int, const char * * argv) {
  auto pool = DatabasePool(new database::Pool("database.sql"));
  auto connection = pool->getConnection().get();

  connection.execute("CREATE TABLE IF NOT EXISTS Request (request TEXT);");

  http::asio::io_context io_context;

  auto listener = std::make_shared<http::Listener>(io_context);

  listener->listen(http::tcp::endpoint(http::asio::ip::make_address("127.0.0.1"), 8080))
    ->accept<MySession>(std::make_tuple(pool));

  const size_t THREAD_COUNT = 4;
  std::vector<std::thread> threads;
  threads.reserve(THREAD_COUNT);
  for (auto i = THREAD_COUNT - 1; i > 0; --i) {
    threads.emplace_back([&io_context]{
      io_context.run();
    });
  }

  boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
  signals.async_wait([&io_context, &listener](const std::error_code error_code, const int signal){
    listener->close();
    io_context.stop();
  });

  io_context.run();

  for (auto & thread : threads) {
    thread.join();
  }

  return 0;
}

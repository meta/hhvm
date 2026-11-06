/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source path is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the path LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <folly/json/dynamic.h>
#include <folly/init/Init.h>
#include <folly/json/json.h>

#include "hphp/runtime/base/watchman.h"

#include "hphp/util/optional.h"

using namespace HPHP;

int main(int argc, char** argv) {
  folly::Init _{&argc, &argv};

  if (argc < 2) {
    std::cout << argv[0] << " <root> [sock]" << std::endl;
    return 2;
  }

  std::string root = argv[1];
  HPHP::Optional<std::string> sock;
  if (argc > 2) {
    sock = {{argv[2]}};
  }

  auto watchman = Watchman::get(root, sock);
  std::vector<std::unique_ptr<Watchman::Subscription>> subscriptions;
  std::string clock;

  std::string line;
  folly::dynamic queryObj;
  std::cout << "Type a query, or 'b' for a basic query" << std::endl
            << "Typing 's' will subscribe to the last query sent" << std::endl;
  while (!std::cin.eof()) {
    std::getline(std::cin, line);
    if (line.empty()) {
      continue;
    }
    if (line == "b") {
      queryObj = folly::dynamic::object(
          "fields", folly::dynamic::array("name", "exists"));
    } else if (line == "s") {
      if (!queryObj.isObject()) {
        std::cout << "No query to subscribe to." << std::endl;
        continue;
      }
      subscriptions.push_back(watchman->subscribe(
          queryObj, [](folly::Try<folly::dynamic>&& results) {
            if (results.hasValue()) {
              std::cout << "Subscription got: "
                        << folly::toPrettyJson(results.value()) << std::endl;
            } else if (results.hasException()) {
              std::cout << "Subscription error: " << results.exception()
                        << std::endl;
            }
          }));
      std::cout << "Subscribed using query " << folly::toPrettyJson(queryObj)
                << std::endl;
      continue;
    } else {
      try {
        queryObj = folly::parseJson(line);
      } catch (const std::runtime_error& e) {
        std::cout << "Couldn't parse " << line << ": " << e.what() << std::endl;
        continue;
      }
    }
    if (!clock.empty()) {
      queryObj["since"] = clock;
    }
    if (!queryObj.isObject()) {
      continue;
    }
    try {
      folly::dynamic res = watchman->query(queryObj).get();
      std::cout << folly::toPrettyJson(res) << std::endl;
      clock = res["clock"].asString();
    } catch (const folly::AsyncSocketException& e) {
      std::cout << e.what() << std::endl;
    }
  }
}

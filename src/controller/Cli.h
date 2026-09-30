#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "controller/Controller.h"

namespace usp {

// Interactive console for manual testing (reads commands from stdin).
class Cli {
 public:
  explicit Cli(Controller& controller) : controller_(controller) {}
  // Runs until "quit", EOF or `stop` becomes true.
  void run(const std::atomic<bool>& stop);
  // Executes one command line; returns false for "quit".
  bool execute(const std::string& line);

  static std::vector<std::string> tokenize(const std::string& line, std::string& error);

 private:
  std::string resolve_agent();
  void print_help();

  Controller& controller_;
  std::string current_agent_;
};

}  // namespace usp

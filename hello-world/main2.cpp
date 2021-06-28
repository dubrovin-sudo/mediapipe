#include <emscripten/bind.h>
#include "mediapipe/framework/port/logging.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/calculator_graph.h"

using namespace emscripten;

namespace mediapipe {
  std::string helloName(std::string name) {
    return "hello " + name;
  }

  absl::Status PrintHelloWorld() {
    CalculatorGraphConfig config =
      ParseTextProtoOrDie<CalculatorGraphConfig>(R"pb(
        input_stream: "in"
        output_stream: "out"
        node {
          calculator: "PassThroughCalculator"
          input_stream: "in"
          output_stream: "out1"
        }
        node {
          calculator: "PassThroughCalculator"
          input_stream: "out1"
          output_stream: "out"
        }
      )pb");

    // CalculatorGraph graph;
    // MP_RETURN_IF_ERROR(graph.Initialize(config));
    MP_RETURN_IF_ERROR(absl::InvalidArgumentError("testing MP_RETURN_IF_ERROR macro"));
    return absl::OkStatus();
  }


  std::string runMPGraph() {
    absl::Status status = PrintHelloWorld();

    return status.ToString();
  }



} // namespace mediapipe


int main() {}

EMSCRIPTEN_BINDINGS(Hello_World_Simple) {
  // function("PrintHelloWorld", &mediapipe::PrintHelloWorld);
  function("helloName", &mediapipe::helloName);
//   function("helloName", &mediapipe::helloName);
  function("runMPGraph", &mediapipe::runMPGraph);
  
}
#include <emscripten/bind.h>
#include "mediapipe/framework/port/logging.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/calculator_graph.h"

using namespace emscripten;

// namespace mediapipe {
std::string helloName(std::string name) {
  return "hello " + name;
}

absl::Status PrintHelloWorld() {
  mediapipe::CalculatorGraphConfig config =
    mediapipe::ParseTextProtoOrDie<mediapipe::CalculatorGraphConfig>(R"pb(
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

  mediapipe::CalculatorGraph graph;
  MP_RETURN_IF_ERROR(graph.Initialize(config));
  ASSIGN_OR_RETURN(mediapipe::OutputStreamPoller poller,
                   graph.AddOutputStreamPoller("out"));

  MP_RETURN_IF_ERROR(graph.StartRun({}));
  
  for (int i = 0; i < 1; ++i) {
    MP_RETURN_IF_ERROR(graph.AddPacketToInputStream(
        "in", mediapipe::MakePacket<std::string>("Wasmup!").At(mediapipe::Timestamp(i))));
  }

  // Close the input stream "in".
  MP_RETURN_IF_ERROR(graph.CloseInputStream("in"));

  mediapipe::Packet packet;
  // // Get the output packets std::string.
  // while (poller.Next(&packet)) {
  //   LOG(INFO) << packet.Get<std::string>();
  // }

  return graph.WaitUntilDone();

  MP_RETURN_IF_ERROR(absl::InvalidArgumentError("testing MP_RETURN_IF_ERROR macro"));
  // return absl::OkStatus();
}


std::string runMPGraph() {
  absl::Status status = PrintHelloWorld();

  if (!status.ok()) {
    LOG(WARNING) << "Unexpected error " << status;
  }

  return status.ToString();
}



// } // namespace mediapipe


int main() {}

EMSCRIPTEN_BINDINGS(Hello_World_Simple) {
  // function("PrintHelloWorld", &mediapipe::PrintHelloWorld);
  function("helloName", &helloName);
//   function("helloName", &helloName);
  function("runMPGraph", &runMPGraph);
  
}
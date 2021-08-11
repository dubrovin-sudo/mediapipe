#include <emscripten/bind.h>
#include "mediapipe/framework/port/logging.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/calculator_graph.h"
#include "mediapipe/gpu/gl_context_internal.h"


#include "mediapipe/gpu/gl_calculator_helper.h"
#include "mediapipe/gpu/gpu_buffer.h"
#include "mediapipe/gpu/gpu_shared_data_internal.h"

#include "mediapipe/framework/formats/image_frame.h"

#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/calculator_graph.h"



#include "mediapipe/gpu/gl_simple_calculator.h"
#include "mediapipe/gpu/gl_simple_shaders.h" // GLES_VERSION_COMPAT
#include "mediapipe/gpu/shader_util.h"
#include "mediapipe/framework/formats/detection.pb.h"
#include "mediapipe/framework/formats/location_data.pb.h"

using namespace emscripten;

constexpr char kMaskGpuTag[] = "MASK_GPU";


enum { ATTRIB_VERTEX, ATTRIB_TEXTURE_POSITION, NUM_ATTRIBUTES };


namespace mediapipe {


class RenderGPUBufferToCanvasCalculator: public CalculatorBase {
  public:
  RenderGPUBufferToCanvasCalculator() : initialized_(false) {}
  RenderGPUBufferToCanvasCalculator(const RenderGPUBufferToCanvasCalculator&) = delete;
  RenderGPUBufferToCanvasCalculator& operator=(const RenderGPUBufferToCanvasCalculator&) = delete;
  ~RenderGPUBufferToCanvasCalculator() override = default;

  static absl::Status GetContract(CalculatorContract* cc);
  absl::Status Open(CalculatorContext* cc) override;
  absl::Status Process(CalculatorContext* cc) override;
  absl::Status Close(CalculatorContext* cc) override;

  absl::Status GlBind() { return absl::OkStatus(); }

  void GetOutputDimensions(int src_width, int src_height,
                                   int* dst_width, int* dst_height) {
    *dst_width = src_width;
    *dst_height = src_height;
  }

  virtual GpuBufferFormat GetOutputFormat() { return GpuBufferFormat::kBGRA32; } 
  
  absl::Status GlSetup();
  absl::Status GlRender(const GlTexture& src,
                        const GlTexture& dst);
  absl::Status GlTeardown();

 protected:
  template <typename F>
  auto RunInGlContext(F&& f)
      -> decltype(std::declval<GlCalculatorHelper>().RunInGlContext(f)) {
    return helper_.RunInGlContext(std::forward<F>(f));
  }

  GlCalculatorHelper helper_;
  bool initialized_;
  
  private:
  GLuint program_ = 0;
  GLint frame_;
};

REGISTER_CALCULATOR(RenderGPUBufferToCanvasCalculator);

absl::Status RenderGPUBufferToCanvasCalculator::GetContract(CalculatorContract* cc) {
  TagOrIndex(&cc->Inputs(), "VIDEO", 0).Set<GpuBuffer>();
  TagOrIndex(&cc->Outputs(), "VIDEO", 0).Set<GpuBuffer>();
  // Currently we pass GL context information and other stuff as external
  // inputs, which are handled by the helper.
  return GlCalculatorHelper::UpdateContract(cc);
}

absl::Status RenderGPUBufferToCanvasCalculator::Open(CalculatorContext* cc) {
  // Inform the framework that we always output at the same timestamp
  // as we receive a packet at.
  cc->SetOffset(mediapipe::TimestampDiff(0));

  // Let the helper access the GL context information.
  return helper_.Open(cc);
}

absl::Status RenderGPUBufferToCanvasCalculator::Process(CalculatorContext* cc) {
  return RunInGlContext([this, cc]() -> absl::Status {
    const auto& input = TagOrIndex(cc->Inputs(), "VIDEO", 0).Get<GpuBuffer>();
    if (!initialized_) {
      MP_RETURN_IF_ERROR(GlSetup());
      initialized_ = true;
    }

    auto src = helper_.CreateSourceTexture(input);
    int dst_width;
    int dst_height;
    GetOutputDimensions(src.width(), src.height(), &dst_width, &dst_height);
    auto dst = helper_.CreateDestinationTexture(dst_width, dst_height,
                                                GetOutputFormat());

    helper_.BindFramebuffer(dst);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(src.target(), src.name());

    MP_RETURN_IF_ERROR(GlBind());
    // Run core program.
    MP_RETURN_IF_ERROR(GlRender(src, dst));

    glBindTexture(src.target(), 0);

    glFlush();

    auto output = dst.GetFrame<GpuBuffer>();

    src.Release();
    dst.Release();

    TagOrIndex(&cc->Outputs(), "VIDEO", 0)
        .Add(output.release(), cc->InputTimestamp());

    return absl::OkStatus();
  });
}

absl::Status RenderGPUBufferToCanvasCalculator::Close(CalculatorContext* cc) {
  return RunInGlContext([this]() -> absl::Status { return GlTeardown(); });
}

absl::Status RenderGPUBufferToCanvasCalculator::GlSetup() {
  LOG(INFO) << "RenderGPUBufferToCanvasCalculator::GlSetup NUM_ATTRIBUTES:" << NUM_ATTRIBUTES << " ATTRIB_VERTEX:" << ATTRIB_VERTEX << " ATTRIB_TEXTURE_POSITION:" << ATTRIB_TEXTURE_POSITION << "\n";

  const GLint attr_location[NUM_ATTRIBUTES] = {
      ATTRIB_VERTEX,
      ATTRIB_TEXTURE_POSITION,
  };

  const GLchar* attr_name[NUM_ATTRIBUTES] = {
      "position",
      "texture_coordinate",
  };

  const GLchar* frag_src = GLES_VERSION_COMPAT
      R"(
  #if __VERSION__ < 130
    #define in varying
  #endif  // __VERSION__ < 130

  #ifdef GL_ES
    #define fragColor gl_FragColor
    precision highp float;
  #else
    #define lowp
    #define mediump
    #define highp
    #define texture2D texture
    out vec4 fragColor;
  #endif  // defined(GL_ES)

    in vec2 sample_coordinate;
    uniform sampler2D video_frame;
    const highp vec3 W = vec3(0.2125, 0.7154, 0.0721);

    void main() {
      vec4 color = texture2D(video_frame, sample_coordinate);
      // float luminance = dot(color.rgb, W);
      // fragColor.rgb = vec3(luminance);
      // fragColor.rgb = vec3(0.0, 0.0, 1.0);
      fragColor.rgb = color.rgb;

      // fragColor.a = 1.0;
      fragColor.a = color.a;
    }

  )";

  // Creates a GLSL program by compiling and linking the provided shaders.
  // Also obtains the locations of the requested attributes.
  auto glStatus = GlhCreateProgram(kBasicVertexShader, frag_src, NUM_ATTRIBUTES,
                   (const GLchar**)&attr_name[0], attr_location, &program_);
  
  if (glStatus != GL_TRUE) {
    LOG(ERROR) << "GlhCreateProgram failed";
  } else {
    LOG(INFO) << "GlhCreateProgram success";
  }
  
  RET_CHECK(program_) << "Problem initializing the program.";
  frame_ = glGetUniformLocation(program_, "video_frame");
  
  return absl::OkStatus();
}

absl::Status RenderGPUBufferToCanvasCalculator::GlRender(const GlTexture& src, const GlTexture& dst) {
  static const GLfloat square_vertices[] = {
      -1.0f, -1.0f,  // bottom left
      1.0f,  -1.0f,  // bottom right
      -1.0f, 1.0f,   // top left
      1.0f,  1.0f,   // top right
  };
  static const GLfloat texture_vertices[] = {
      0.0f, 1.0f,  // top left
      1.0f, 1.0f,  // top right
      0.0f, 0.0f,  // bottom left
      1.0f, 0.0f,  // bottom right
  };

  glBindFramebuffer(GL_FRAMEBUFFER, 0); // binding to canvas

  // program
  glUseProgram(program_);
  glUniform1i(frame_, 1);

  // vertex storage
  GLuint vbo[2];
  glGenBuffers(2, vbo);
  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);

  // vbo 0
  glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
  glBufferData(GL_ARRAY_BUFFER, 4 * 2 * sizeof(GLfloat), square_vertices,
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(ATTRIB_VERTEX);
  glVertexAttribPointer(ATTRIB_VERTEX, 2, GL_FLOAT, 0, 0, nullptr);

  // vbo 1
  glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
  glBufferData(GL_ARRAY_BUFFER, 4 * 2 * sizeof(GLfloat), texture_vertices,
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(ATTRIB_TEXTURE_POSITION);
  glVertexAttribPointer(ATTRIB_TEXTURE_POSITION, 2, GL_FLOAT, 0, 0, nullptr);

  // draw
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  // glBindFramebuffer(GL_FRAMEBUFFER, 0); // glBindFramebuffer after draw does not work


  // cleanup
  glDisableVertexAttribArray(ATTRIB_VERTEX);
  glDisableVertexAttribArray(ATTRIB_TEXTURE_POSITION);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glDeleteVertexArrays(1, &vao);
  glDeleteBuffers(2, vbo);


  return absl::OkStatus();  
}

absl::Status RenderGPUBufferToCanvasCalculator::GlTeardown() {
  if (program_) {
    glDeleteProgram(program_);
    program_ = 0;
  }

  return absl::OkStatus();
}

}  // namespace mediapipe

class BoundingBox {
  public:
  float x, y, width, height;

  BoundingBox(float x, float y, float w, float h) {
    this->x = x;
    this->y = y; 
    this->width = w;
    this->height = h;
  }

  BoundingBox() {}
};


class GraphContainer {
  public:
  mediapipe::CalculatorGraph graph;
  bool isGraphInitialized = false;
  mediapipe::GlCalculatorHelper gpu_helper;
  int w = 0;
  int direction = 1;
  int runCounter;
  std::vector<mediapipe::Packet> output_packets;

  uint8* data;

  int* prvTemp;
  std::vector<BoundingBox> boundingBoxes;
  
  std::string graphConfigWithRender = R"pb(
        input_stream: "input_video"
        input_side_packet: "MODEL_SELECTION:model_selection"
        output_stream: "output_video"
        # output_stream: "face_detections"
        output_stream: "segmentation_mask"
        output_stream: "output_video_with_segmentation"
        max_queue_size: 5

        

        node: {
          calculator: "ImageFrameToGpuBufferCalculator"
          input_stream: "input_video"
          output_stream: "input_gpubuffer"
        }

        node {
          calculator: "FlowLimiterCalculator"
          input_stream: "input_gpubuffer"
          input_stream: "FINISHED:output_video"
          input_stream_info: {
            tag_index: "FINISHED"
            back_edge: true
          }
          output_stream: "throttled_input_video"
        }
      
        # Converts RGB images into luminance images, still stored in RGB format.
        # Subgraph that detects faces.
        node {
          calculator: "FaceDetectionShortRangeGpu"
          input_stream: "IMAGE:throttled_input_video"
          output_stream: "DETECTIONS:face_detections"
        }

        node {
          calculator: "SelfieSegmentationGpu"
          input_side_packet: "MODEL_SELECTION:model_selection"
          input_stream: "IMAGE:throttled_input_video"
          output_stream: "SEGMENTATION_MASK:segmentation_mask"
        }

        #node {
        #  calculator: "RecolorCalculator"
        #  input_stream: "IMAGE_GPU:throttled_input_video"
        #  input_stream: "MASK_GPU:segmentation_mask"
        #  output_stream: "IMAGE_GPU:output_video"
        #  node_options: {
        #    [type.googleapis.com/mediapipe.RecolorCalculatorOptions] {
        #      color { r: 0 g: 0 b: 255 }
        #      mask_channel: RED
        #      invert_mask: true
        #      adjust_with_luminance: false
        #    }
        #  }
        #}

        node: {
          calculator: "RenderGPUBufferToCanvasCalculator"
          #input_stream: "VIDEO:output_video_with_segmentation"
          #input_stream: "VIDEO:throttled_input_video"
          input_stream: "VIDEO:segmentation_mask"
          output_stream: "VIDEO:output_video"
        }
      )pb";
  

  absl::Status setupGraph() {

    mediapipe::CalculatorGraphConfig config =
      mediapipe::ParseTextProtoOrDie<mediapipe::CalculatorGraphConfig>(graphConfigWithRender);

    MP_RETURN_IF_ERROR(graph.Initialize(config));
    ASSIGN_OR_RETURN(mediapipe::OutputStreamPoller poller,
                    graph.AddOutputStreamPoller("output_video"));
    // ASSIGN_OR_RETURN(mediapipe::OutputStreamPoller pollerDetections,
    //                 graph.AddOutputStreamPoller("detections"));
    ASSIGN_OR_RETURN(auto gpu_resources, mediapipe::GpuResources::Create());
    MP_RETURN_IF_ERROR(graph.SetGpuResources(gpu_resources));
    gpu_helper.InitializeForTest(graph.GetGpuResources().get());
    graph.ObserveOutputStream("output_video", [this](const mediapipe::Packet& p) {
      // LOG(INFO) << "observing packet in output_video output stream";
      // LOG(INFO) << "inside lambda func: packet.Get<std::string>():" << p.Get<std::string>();
      return absl::OkStatus();
    });


    graph.ObserveOutputStream("segmentation_mask", [this](const mediapipe::Packet& p) {
      const mediapipe::GpuBuffer & mask_buffer = p.Get<mediapipe::GpuBuffer>();
      LOG(INFO) << "main2.cc mask_buffer width:" << mask_buffer.width() << " height:" << mask_buffer.height();
      return absl::OkStatus();
    });

    boundingBoxes.resize(1);
    // graph.ObserveOutputStream("segmentation_mask", [this](const mediapipe::Packet& p) {
    //   const mediapipe::GpuBuffer & mask_buffer = p.Get<mediapipe::GpuBuffer>();
    // });

    graph.ObserveOutputStream("face_detections", [this](const mediapipe::Packet& p) {
      const auto& detections = p.Get<std::vector<mediapipe::Detection>>();

      const int n = detections.size();

      this->boundingBoxes.resize(n);
      float xmin, ymin, width, height;

      for (int i = 0; i < n; i ++) {
        mediapipe::LocationData loc = detections[i].location_data();

        if (loc.format() == mediapipe::LocationData::RELATIVE_BOUNDING_BOX) {
          auto boundingBox = loc.relative_bounding_box();
          xmin = boundingBox.xmin();
          ymin = boundingBox.ymin();
          width = boundingBox.width();
          height = boundingBox.height();
          this->boundingBoxes[i].x = xmin;
          this->boundingBoxes[i].y = ymin;
          this->boundingBoxes[i].width = width;
          this->boundingBoxes[i].height = height;
        }

        LOG(INFO) <<  "main2.cc xmin:" << xmin << " ymin:" << ymin << " width:" << width << " height:" << height;
      }

      LOG(INFO) << "main2.cc detections size:" << n;

      for (const mediapipe::Detection & d: detections) {
        LOG(INFO) << "main2.cc has_detection_id:" << d.has_detection_id(); // << " detection_id:" << d.detection_id() << " score:" << d.score();
      }

      return absl::OkStatus();
    });


    MP_RETURN_IF_ERROR(graph.StartRun({}));

    return absl::OkStatus();
  }

  absl::Status init() {
    isGraphInitialized = false;
    w = 0;
    direction = 1;
    runCounter = 0;
    prvTemp = nullptr;

    FILE* ret = freopen("assets/in.txt", "r", stdin);
    if (ret == nullptr) {
      LOG(ERROR) << "could not open assets/in.txt";
    }
    int n;
    while (std::cin >> n) {
      LOG(INFO) << "From file: " << n;
    }

    return this->setupGraph();
  }

  GraphContainer(uint32 maxWidth, uint32 maxHeight) {  
    data = (uint8*)malloc(4*480*640);
    
    absl::Status status = this->init();
    if (!status.ok()) {
      LOG(ERROR) << status;
    }
  }

  GraphContainer() {  
    data = (uint8*)malloc(4*480*640);

    absl::Status status = this->init();
    if (!status.ok()) {
      LOG(ERROR) << status;
    }
  }


  absl::Status webglCanvasDraw(uint8* imgData, int imgSize) {

    int* temp = (int *) malloc(5000);
    if (prvTemp == nullptr) prvTemp = temp;
    printf("temp: %d, change: %d \n", static_cast<int *>(temp), (temp - prvTemp));
    prvTemp = temp;
    free(temp);


    uint8* imgPtr = imgData;
    
    w += direction;
    if (w == 500 || w == 0) direction = -direction;

    // LOG(INFO) << "w:" << w;
    LOG(INFO) << "imgSize:" << imgSize << "(4*480*640):" << (4*480*640);

    for (int ptr = 0; ptr < imgSize; ptr += 4) {
        // rgba
        data[ptr] = *imgPtr;
        imgPtr ++;
        data[ptr + 1] = *imgPtr;
        imgPtr ++;
        data[ptr + 2] = *imgPtr; //(255*w) / 500;
        imgPtr ++;
        data[ptr + 3] = *imgPtr; 
        imgPtr ++;
    }

    auto imageFrame =
        absl::make_unique<mediapipe::ImageFrame>(mediapipe::ImageFormat::SRGBA, 640, 480,
                                    mediapipe::ImageFrame::kGlDefaultAlignmentBoundary);
    int img_data_size = 640 * 480 * 4;
    std::memcpy(imageFrame->MutablePixelData(), data,
                img_data_size);
    size_t frame_timestamp_us = runCounter * 1e6;
    runCounter ++;

    MP_RETURN_IF_ERROR(          
      graph.AddPacketToInputStream(
        "input_video",
        mediapipe::Adopt(
          imageFrame.release()
        ).At(
          mediapipe::Timestamp(frame_timestamp_us)
        )
      )
    ); 

    MP_RETURN_IF_ERROR(
      gpu_helper.RunInGlContext(
        [this]() -> absl::Status {
          
          glFlush();
          
          MP_RETURN_IF_ERROR(
            this->graph.WaitUntilIdle()
          );

          return absl::OkStatus();
        }
      )
    );

    // delete imageFrame;
    // delete data;
    
    // MP_RETURN_IF_ERROR(graph.WaitUntilDone());
    return absl::OkStatus();
  }

  std::string run(uintptr_t imgData, int imgSize) {
    // absl::Status status = this->webglCanvasDraw(imgData, imgSize);

    absl::Status status = this->webglCanvasDraw(reinterpret_cast<uint8*>(imgData), imgSize);

    if (!status.ok()) {
      LOG(WARNING) << "Unexpected error " << status;
    }

    return status.ToString();
  }

  absl::Status cleanGraph() {
    MP_RETURN_IF_ERROR(graph.CloseInputStream("input_video"));
    MP_RETURN_IF_ERROR(graph.CloseAllPacketSources());
    return absl::OkStatus();
  }

  ~GraphContainer() {
    absl::Status stat = cleanGraph();
    if (!stat.ok()) {
      LOG(ERROR) << stat;
    }
  }
};


int main() {}

EMSCRIPTEN_BINDINGS(Hello_World_Simple) {
  class_<GraphContainer>("GraphContainer")
    .constructor()
    .constructor<int, int>()
    .function("run", &GraphContainer::run)
    .property("boundingBoxes", &GraphContainer::boundingBoxes)
    ;
  class_<BoundingBox>("BoundingBox")
    // .constructor<float, float, float, float>()
    .property("x", &BoundingBox::x)
    .property("y", &BoundingBox::y)
    .property("width", &BoundingBox::width)
    .property("height", &BoundingBox::height)
    ;
  register_vector<BoundingBox>("vector<BoundingBox>");
  
}
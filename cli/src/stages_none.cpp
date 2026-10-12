// open_stages() for builds without ONNX Runtime and OpenCV
// (SPG_WITH_DEPS=OFF): there are no detectors to run.
#include "image_io.hpp"
#include "stages.hpp"

namespace spg::cli {

OpenedStages open_stages(const ModelPaths& /*models*/, const PolicyConfig& /*policy*/,
                         const StageOptions& /*options*/) {
  throw CliError("this spg-cli was built without ONNX Runtime and OpenCV (SPG_WITH_DEPS=OFF); "
                 "rebuild with them to run the pipeline");
}

}  // namespace spg::cli

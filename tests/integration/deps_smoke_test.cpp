// Proves the prebuilt ONNX Runtime and OpenCV pinned in deps/manifest.json
// link, load at runtime and are the versions the manifest names.
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {

bool has_provider(const std::vector<std::string>& providers, std::string_view name) {
  return std::find(providers.begin(), providers.end(), name) != providers.end();
}

}  // namespace

TEST_CASE("ONNX Runtime loads and matches the manifest version", "[deps][onnxruntime]") {
  const std::string_view version = OrtGetApiBase()->GetVersionString();
  CHECK(version == SPG_EXPECTED_ORT_VERSION);

  const std::vector<std::string> providers = Ort::GetAvailableProviders();
  CHECK(has_provider(providers, "CPUExecutionProvider"));
#if defined(__APPLE__)
  CHECK(has_provider(providers, "CoreMLExecutionProvider"));
#endif

  // Creating an environment exercises the runtime beyond symbol resolution.
  const Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spg-deps-tests");
  const Ort::SessionOptions options;
  SUCCEED("Ort::Env and Ort::SessionOptions created");
}

TEST_CASE("OpenCV links, matches the manifest version and blurs", "[deps][opencv]") {
  CHECK(cv::getVersionString() == SPG_EXPECTED_OPENCV_VERSION);

  // A single bright pixel on black spreads under a Gaussian blur, keeps its
  // energy (within rounding) and leaves distant pixels untouched.
  cv::Mat impulse = cv::Mat::zeros(15, 15, CV_8UC1);
  impulse.at<unsigned char>(7, 7) = 255;
  cv::Mat blurred;
  cv::GaussianBlur(impulse, blurred, cv::Size(5, 5), 1.0);

  CHECK(blurred.at<unsigned char>(7, 7) > 0);
  CHECK(blurred.at<unsigned char>(7, 7) < 255);
  CHECK(blurred.at<unsigned char>(7, 8) > 0);
  CHECK(blurred.at<unsigned char>(6, 7) > 0);
  CHECK(blurred.at<unsigned char>(0, 0) == 0);
  CHECK(blurred.at<unsigned char>(7, 12) == 0);
  const double energy = cv::sum(blurred)[0];
  CHECK(energy > 235.0);
  CHECK(energy < 275.0);
}

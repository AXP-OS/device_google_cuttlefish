/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef CUTTLEFISH_COMMON_COMMON_LIBS_METADATA_DISPLAY_PROPERTIES_H_
#define CUTTLEFISH_COMMON_COMMON_LIBS_METADATA_DISPLAY_PROPERTIES_H_

#include "common/libs/auto_resources/auto_resources.h"

namespace avd {

class DisplayProperties {
 public:
  DisplayProperties() :
      x_res_(1280),
      y_res_(720),
      bits_per_pixel_(32),
      dpi_(160),
      default_(true) {
        config_.SetToString("1280x720x32x160");
      }

  void Parse(const char* value);

  int GetXRes() const { return x_res_; }
  int GetYRes() const { return y_res_; }
  int GetBitsPerPixel() const { return bits_per_pixel_; }
  int GetDpi() const { return dpi_; }
  bool IsDefault() const { return default_; }
  const char* GetConfig() const { return config_.data(); }

 private:
  // Screen width in pixels
  int x_res_;
  // Screen height in pixels
  int y_res_;
  // Depth of the screen (obsolete)
  int bits_per_pixel_;
  // Pixels per inch
  int dpi_;
  // Default
  bool default_;
  // Unparsed configuration
  AutoFreeBuffer config_;
};

}  // namespace avd
#endif  // CUTTLEFISH_COMMON_COMMON_LIBS_METADATA_DISPLAY_PROPERTIES_H_

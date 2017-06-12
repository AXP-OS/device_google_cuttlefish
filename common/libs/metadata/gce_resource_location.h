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
#ifndef CUTTLEFISH_COMMON_COMMON_LIBS_METADATA_GCE_RESOURCE_LOCATION_H_
#define CUTTLEFISH_COMMON_COMMON_LIBS_METADATA_GCE_RESOURCE_LOCATION_H_

class GceResourceLocation {
 public:
  static const char* const kInitialMetadataPath;
  static const char* const kInitialFstabPath;
  static const char* const kDevicePersonalitiesPath;
};

#endif  // CUTTLEFISH_COMMON_COMMON_LIBS_METADATA_GCE_RESOURCE_LOCATION_H_

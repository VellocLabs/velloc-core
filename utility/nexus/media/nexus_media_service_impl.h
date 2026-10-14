// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_NEXUS_MEDIA_SERVICE_IMPL_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_NEXUS_MEDIA_SERVICE_IMPL_H_

#include <optional>
#include <string>

#include "custom_browser/browser/nexus/mojom/nexus_media.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace nexus {

class NexusMediaServiceImpl : public mojom::NexusMediaService {
 public:
  explicit NexusMediaServiceImpl(
      mojo::PendingReceiver<mojom::NexusMediaService> receiver);
  ~NexusMediaServiceImpl() override;

  NexusMediaServiceImpl(const NexusMediaServiceImpl&) = delete;
  NexusMediaServiceImpl& operator=(const NexusMediaServiceImpl&) = delete;

  void ReadImage(mojom::MediaReadOptionsPtr options,
                 ReadImageCallback callback) override;
  void ReadNotebook(mojom::MediaReadOptionsPtr options,
                    ReadNotebookCallback callback) override;
  void ReadPdf(mojom::MediaReadOptionsPtr options,
               ReadPdfCallback callback) override;

 private:
  mojo::Receiver<mojom::NexusMediaService> receiver_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_NEXUS_MEDIA_SERVICE_IMPL_H_

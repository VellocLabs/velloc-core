// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/media/nexus_media_service_impl.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "custom_browser/utility/nexus/media/image_processor.h"
#include "custom_browser/utility/nexus/media/notebook_processor.h"
#include "custom_browser/utility/nexus/media/pdf_processor.h"

namespace nexus {
namespace {

template <typename Callback>
void ReplyMedia(Callback callback, MediaReadOutcome outcome) {
  std::move(callback).Run(outcome.ok, std::move(outcome.result),
                          std::move(outcome.error_code),
                          std::move(outcome.error_message));
}

}  // namespace

NexusMediaServiceImpl::NexusMediaServiceImpl(
    mojo::PendingReceiver<mojom::NexusMediaService> receiver) {
  receiver_.Bind(std::move(receiver));
}

NexusMediaServiceImpl::~NexusMediaServiceImpl() = default;

void NexusMediaServiceImpl::ReadImage(mojom::MediaReadOptionsPtr options,
                                      ReadImageCallback callback) {
  if (!options) {
    MediaReadOutcome outcome = MakeMediaError("invalid_args", "missing options");
    ReplyMedia(std::move(callback), std::move(outcome));
    return;
  }
  const std::string mime_type = options->mime_type.value_or("image/png");
  auto reply_runner = base::SequencedTaskRunner::GetCurrentDefault();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadImageMedia, options->path, mime_type),
      base::BindPostTask(reply_runner,
                         base::BindOnce(&ReplyMedia<ReadImageCallback>,
                                        std::move(callback))));
}

void NexusMediaServiceImpl::ReadNotebook(mojom::MediaReadOptionsPtr options,
                                         ReadNotebookCallback callback) {
  if (!options) {
    MediaReadOutcome outcome = MakeMediaError("invalid_args", "missing options");
    ReplyMedia(std::move(callback), std::move(outcome));
    return;
  }
  auto reply_runner = base::SequencedTaskRunner::GetCurrentDefault();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadNotebookMedia, options->path),
      base::BindPostTask(reply_runner,
                         base::BindOnce(&ReplyMedia<ReadNotebookCallback>,
                                        std::move(callback))));
}

void NexusMediaServiceImpl::ReadPdf(mojom::MediaReadOptionsPtr options,
                                    ReadPdfCallback callback) {
  if (!options) {
    MediaReadOutcome outcome = MakeMediaError("invalid_args", "missing options");
    ReplyMedia(std::move(callback), std::move(outcome));
    return;
  }
  // PDFium's Chromium wrapper requires a current SingleThreadTaskRunner for V8
  // setup. Run it on the utility service sequence (child process, not browser UI)
  // instead of a generic ThreadPool worker.
  MediaReadOutcome outcome = ReadPdfMedia(options->path, options->pages);
  ReplyMedia(std::move(callback), std::move(outcome));
}

}  // namespace nexus

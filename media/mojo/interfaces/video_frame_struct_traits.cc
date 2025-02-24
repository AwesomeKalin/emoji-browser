// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/mojo/interfaces/video_frame_struct_traits.h"

#include <utility>
#include <vector>

#include "base/logging.h"
#include "build/build_config.h"
#include "media/mojo/common/mojo_shared_buffer_video_frame.h"
#include "mojo/public/cpp/base/time_mojom_traits.h"
#include "mojo/public/cpp/base/values_mojom_traits.h"
#include "mojo/public/cpp/system/handle.h"
#include "mojo/public/cpp/system/platform_handle.h"
#include "ui/gfx/mojo/color_space_mojom_traits.h"

namespace mojo {

namespace {

media::mojom::VideoFrameDataPtr MakeVideoFrameData(
    const media::VideoFrame* input) {
  if (input->metadata()->IsTrue(media::VideoFrameMetadata::END_OF_STREAM)) {
    return media::mojom::VideoFrameData::NewEosData(
        media::mojom::EosVideoFrameData::New());
  }

  if (input->storage_type() == media::VideoFrame::STORAGE_MOJO_SHARED_BUFFER) {
    const media::MojoSharedBufferVideoFrame* mojo_frame =
        static_cast<const media::MojoSharedBufferVideoFrame*>(input);

    // TODO(https://crbug.com/803136): This should duplicate as READ_ONLY, but
    // can't because there is no guarantee that the input handle is sharable as
    // read-only.
    mojo::ScopedSharedBufferHandle dup = mojo_frame->Handle().Clone(
        mojo::SharedBufferHandle::AccessMode::READ_WRITE);
    DCHECK(dup.is_valid());

    return media::mojom::VideoFrameData::NewSharedBufferData(
        media::mojom::SharedBufferVideoFrameData::New(
            std::move(dup), mojo_frame->MappedSize(),
            mojo_frame->stride(media::VideoFrame::kYPlane),
            mojo_frame->stride(media::VideoFrame::kUPlane),
            mojo_frame->stride(media::VideoFrame::kVPlane),
            mojo_frame->PlaneOffset(media::VideoFrame::kYPlane),
            mojo_frame->PlaneOffset(media::VideoFrame::kUPlane),
            mojo_frame->PlaneOffset(media::VideoFrame::kVPlane)));
  }

#if defined(OS_LINUX)
  if (input->storage_type() == media::VideoFrame::STORAGE_DMABUFS) {
    std::vector<mojo::ScopedHandle> dmabuf_fds;

    const size_t num_planes = media::VideoFrame::NumPlanes(input->format());
    dmabuf_fds.reserve(num_planes);
    for (size_t i = 0; i < num_planes; i++) {
      const int dmabuf_fd = HANDLE_EINTR(dup(input->DmabufFds()[i].get()));
      dmabuf_fds.emplace_back(mojo::WrapPlatformFile(dmabuf_fd));
      DCHECK(dmabuf_fds.back().is_valid());
    }

    return media::mojom::VideoFrameData::NewDmabufData(
        media::mojom::DmabufVideoFrameData::New(std::move(dmabuf_fds)));
  }
#endif

  if (input->HasTextures()) {
    std::vector<gpu::MailboxHolder> mailbox_holder(
        media::VideoFrame::kMaxPlanes);
    size_t num_planes = media::VideoFrame::NumPlanes(input->format());
    for (size_t i = 0; i < num_planes; i++)
      mailbox_holder[i] = input->mailbox_holder(i);
    return media::mojom::VideoFrameData::NewMailboxData(
        media::mojom::MailboxVideoFrameData::New(
            std::move(mailbox_holder), std::move(input->ycbcr_info())));
  }

  NOTREACHED() << "Unsupported VideoFrame conversion";
  return nullptr;
}

}  // namespace

// static
media::mojom::VideoFrameDataPtr StructTraits<media::mojom::VideoFrameDataView,
                                             scoped_refptr<media::VideoFrame>>::
    data(const scoped_refptr<media::VideoFrame>& input) {
  return media::mojom::VideoFrameDataPtr(MakeVideoFrameData(input.get()));
}

// static
bool StructTraits<media::mojom::VideoFrameDataView,
                  scoped_refptr<media::VideoFrame>>::
    Read(media::mojom::VideoFrameDataView input,
         scoped_refptr<media::VideoFrame>* output) {
  // View of the |data| member of the input media::mojom::VideoFrame.
  media::mojom::VideoFrameDataDataView data;
  input.GetDataDataView(&data);

  if (data.is_eos_data()) {
    *output = media::VideoFrame::CreateEOSFrame();
    return !!*output;
  }

  media::VideoPixelFormat format;
  if (!input.ReadFormat(&format))
    return false;

  gfx::Size coded_size;
  if (!input.ReadCodedSize(&coded_size))
    return false;

  gfx::Rect visible_rect;
  if (!input.ReadVisibleRect(&visible_rect))
    return false;

  if (!gfx::Rect(coded_size).Contains(visible_rect))
    return false;

  gfx::Size natural_size;
  if (!input.ReadNaturalSize(&natural_size))
    return false;

  base::TimeDelta timestamp;
  if (!input.ReadTimestamp(&timestamp))
    return false;

  scoped_refptr<media::VideoFrame> frame;
  if (data.is_shared_buffer_data()) {
    media::mojom::SharedBufferVideoFrameDataDataView shared_buffer_data;
    data.GetSharedBufferDataDataView(&shared_buffer_data);

    // TODO(sandersd): Conversion from uint64_t to size_t could cause
    // corruption. Platform-dependent types should be removed from the
    // implementation (limiting to 32-bit offsets is fine).
    frame = media::MojoSharedBufferVideoFrame::Create(
        format, coded_size, visible_rect, natural_size,
        shared_buffer_data.TakeFrameData(),
        shared_buffer_data.frame_data_size(), shared_buffer_data.y_offset(),
        shared_buffer_data.u_offset(), shared_buffer_data.v_offset(),
        shared_buffer_data.y_stride(), shared_buffer_data.u_stride(),
        shared_buffer_data.v_stride(), timestamp);
#if defined(OS_LINUX)
  } else if (data.is_dmabuf_data()) {
    media::mojom::DmabufVideoFrameDataDataView dmabuf_data;
    data.GetDmabufDataDataView(&dmabuf_data);

    std::vector<mojo::ScopedHandle> dmabuf_fds_data;
    if (!dmabuf_data.ReadDmabufFds(&dmabuf_fds_data))
      return false;

    const size_t num_planes = media::VideoFrame::NumPlanes(format);

    std::vector<int> strides =
        media::VideoFrame::ComputeStrides(format, coded_size);
    std::vector<size_t> buffer_sizes;
    buffer_sizes.reserve(num_planes);
    for (size_t i = 0; i < num_planes; i++) {
      buffer_sizes.emplace_back(static_cast<size_t>(
          media::VideoFrame::PlaneSize(format, i, coded_size).GetArea()));
    }

    DCHECK_EQ(num_planes, dmabuf_fds_data.size());
    DCHECK_EQ(num_planes, strides.size());
    DCHECK_EQ(num_planes, buffer_sizes.size());

    auto layout = media::VideoFrameLayout::CreateWithStrides(
        format, coded_size, std::move(strides), std::move(buffer_sizes));
    if (!layout)
      return false;

    std::vector<base::ScopedFD> dmabuf_fds;
    dmabuf_fds.reserve(num_planes);
    for (size_t i = 0; i < num_planes; i++) {
      base::PlatformFile platform_file;
      mojo::UnwrapPlatformFile(std::move(dmabuf_fds_data[i]), &platform_file);
      dmabuf_fds.emplace_back(platform_file);
      DCHECK(dmabuf_fds.back().is_valid());
    }
    frame = media::VideoFrame::WrapExternalDmabufs(
        *layout, visible_rect, natural_size, std::move(dmabuf_fds), timestamp);
#endif
  } else if (data.is_mailbox_data()) {
    media::mojom::MailboxVideoFrameDataDataView mailbox_data;
    data.GetMailboxDataDataView(&mailbox_data);

    std::vector<gpu::MailboxHolder> mailbox_holder;
    if (!mailbox_data.ReadMailboxHolder(&mailbox_holder))
      return false;

    gpu::MailboxHolder mailbox_holder_array[media::VideoFrame::kMaxPlanes];
    for (size_t i = 0; i < media::VideoFrame::kMaxPlanes; i++)
      mailbox_holder_array[i] = mailbox_holder[i];

    base::Optional<gpu::VulkanYCbCrInfo> ycbcr_info;
    if (!mailbox_data.ReadYcbcrData(&ycbcr_info))
      return false;

    frame = media::VideoFrame::WrapNativeTextures(
        format, mailbox_holder_array, media::VideoFrame::ReleaseMailboxCB(),
        coded_size, visible_rect, natural_size, timestamp);
    frame->set_ycbcr_info(ycbcr_info);
  } else {
    // TODO(sandersd): Switch on the union tag to avoid this ugliness?
    NOTREACHED();
    return false;
  }

  if (!frame)
    return false;

  base::Value metadata;
  if (!input.ReadMetadata(&metadata))
    return false;

  frame->metadata()->MergeInternalValuesFrom(metadata);

  gfx::ColorSpace color_space;
  if (!input.ReadColorSpace(&color_space))
    return false;
  frame->set_color_space(color_space);

  *output = std::move(frame);
  return true;
}

}  // namespace mojo

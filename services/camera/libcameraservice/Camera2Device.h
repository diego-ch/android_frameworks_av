/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA2DEVICE_H
#define ANDROID_SERVERS_CAMERA_CAMERA2DEVICE_H

#include <utils/Condition.h>
#include <utils/Errors.h>
#include <utils/List.h>
#include <utils/Mutex.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <utils/String16.h>
#include <utils/Vector.h>

#include "hardware/camera2.h"

namespace android {

class Camera2Device : public virtual RefBase {
  public:
    Camera2Device(int id);

    ~Camera2Device();

    status_t initialize(camera_module_t *module);

    status_t dump(int fd, const Vector<String16>& args);

    /**
     * Get a pointer to the device's static characteristics metadata buffer
     */
    camera_metadata_t* info();

    /**
     * Submit request for capture. The Camera2Device takes ownership of the
     * passed-in buffer.
     */
    status_t capture(camera_metadata_t *request);

    /**
     * Submit request for streaming. The Camera2Device makes a copy of the
     * passed-in buffer and the caller retains ownership.
     */
    status_t setStreamingRequest(camera_metadata_t *request);

    /**
     * Create an output stream of the requested size and format.
     *
     * If format is CAMERA2_HAL_PIXEL_FORMAT_OPAQUE, then the HAL device selects
     * an appropriate format; it can be queried with getStreamInfo.
     *
     * If format is HAL_PIXEL_FORMAT_COMPRESSED, the size parameter must be
     * equal to the size in bytes of the buffers to allocate for the stream. For
     * other formats, the size parameter is ignored.
     */
    status_t createStream(sp<ANativeWindow> consumer,
            uint32_t width, uint32_t height, int format, size_t size,
            int *id);

    /**
     * Get information about a given stream.
     */
    status_t getStreamInfo(int id,
            uint32_t *width, uint32_t *height, uint32_t *format);

    /**
     * Set stream gralloc buffer transform
     */
    status_t setStreamTransform(int id, int transform);

    /**
     * Delete stream. Must not be called if there are requests in flight which
     * reference that stream.
     */
    status_t deleteStream(int id);

    /**
     * Create a metadata buffer with fields that the HAL device believes are
     * best for the given use case
     */
    status_t createDefaultRequest(int templateId,
            camera_metadata_t **request);

    /**
     * Wait until all requests have been processed. Returns INVALID_OPERATION if
     * the streaming slot is not empty, or TIMED_OUT if the requests haven't
     * finished processing in 10 seconds.
     */
    status_t waitUntilDrained();

  private:

    const int mId;
    camera2_device_t *mDevice;

    camera_metadata_t *mDeviceInfo;
    vendor_tag_query_ops_t *mVendorTagOps;

    /**
     * Queue class for both sending requests to a camera2 device, and for
     * receiving frames from a camera2 device.
     */
    class MetadataQueue: public camera2_request_queue_src_ops_t,
                         public camera2_frame_queue_dst_ops_t {
      public:
        MetadataQueue();
        ~MetadataQueue();

        // Interface to camera2 HAL device, either for requests (device is
        // consumer) or for frames (device is producer)
        const camera2_request_queue_src_ops_t*   getToConsumerInterface();
        void setFromConsumerInterface(camera2_device_t *d);

        // Connect queue consumer endpoint to a camera2 device
        status_t setConsumerDevice(camera2_device_t *d);
        // Connect queue producer endpoint to a camera2 device
        status_t setProducerDevice(camera2_device_t *d);

        const camera2_frame_queue_dst_ops_t* getToProducerInterface();

        // Real interfaces. On enqueue, queue takes ownership of buffer pointer
        // On dequeue, user takes ownership of buffer pointer.
        status_t enqueue(camera_metadata_t *buf);
        status_t dequeue(camera_metadata_t **buf, bool incrementCount = true);
        int      getBufferCount();
        status_t waitForBuffer(nsecs_t timeout);

        // Set repeating buffer(s); if the queue is empty on a dequeue call, the
        // queue copies the contents of the stream slot into the queue, and then
        // dequeues the first new entry. The metadata buffers passed in are
        // copied.
        status_t setStreamSlot(camera_metadata_t *buf);
        status_t setStreamSlot(const List<camera_metadata_t*> &bufs);

        status_t dump(int fd, const Vector<String16>& args);

      private:
        status_t signalConsumerLocked();
        status_t freeBuffers(List<camera_metadata_t*>::iterator start,
                List<camera_metadata_t*>::iterator end);

        camera2_device_t *mDevice;

        Mutex mMutex;
        Condition notEmpty;

        int mFrameCount;

        int mCount;
        List<camera_metadata_t*> mEntries;
        int mStreamSlotCount;
        List<camera_metadata_t*> mStreamSlot;

        bool mSignalConsumer;

        static MetadataQueue* getInstance(
            const camera2_frame_queue_dst_ops_t *q);
        static MetadataQueue* getInstance(
            const camera2_request_queue_src_ops_t *q);

        static int consumer_buffer_count(
            const camera2_request_queue_src_ops_t *q);

        static int consumer_dequeue(const camera2_request_queue_src_ops_t *q,
            camera_metadata_t **buffer);

        static int consumer_free(const camera2_request_queue_src_ops_t *q,
                camera_metadata_t *old_buffer);

        static int producer_dequeue(const camera2_frame_queue_dst_ops_t *q,
                size_t entries, size_t bytes,
                camera_metadata_t **buffer);

        static int producer_cancel(const camera2_frame_queue_dst_ops_t *q,
            camera_metadata_t *old_buffer);

        static int producer_enqueue(const camera2_frame_queue_dst_ops_t *q,
                camera_metadata_t *filled_buffer);

    }; // class MetadataQueue

    MetadataQueue mRequestQueue;
    MetadataQueue mFrameQueue;

    /**
     * Adapter from an ANativeWindow interface to camera2 device stream ops.
     * Also takes care of allocating/deallocating stream in device interface
     */
    class StreamAdapter: public camera2_stream_ops, public virtual RefBase {
      public:
        StreamAdapter(camera2_device_t *d);

        ~StreamAdapter();

        /**
         * Create a HAL device stream of the requested size and format.
         *
         * If format is CAMERA2_HAL_PIXEL_FORMAT_OPAQUE, then the HAL device
         * selects an appropriate format; it can be queried with getFormat.
         *
         * If format is HAL_PIXEL_FORMAT_COMPRESSED, the size parameter must
         * be equal to the size in bytes of the buffers to allocate for the
         * stream. For other formats, the size parameter is ignored.
         */
        status_t connectToDevice(sp<ANativeWindow> consumer,
                uint32_t width, uint32_t height, int format, size_t size);

        status_t release();

        status_t setTransform(int transform);

        // Get stream parameters.
        // Only valid after a successful connectToDevice call.
        int      getId() const     { return mId; }
        uint32_t getWidth() const  { return mWidth; }
        uint32_t getHeight() const { return mHeight; }
        uint32_t getFormat() const { return mFormat; }

        // Dump stream information
        status_t dump(int fd, const Vector<String16>& args);

      private:
        enum {
            ERROR = -1,
            RELEASED = 0,
            ALLOCATED,
            CONNECTED,
            ACTIVE
        } mState;

        sp<ANativeWindow> mConsumerInterface;
        camera2_device_t *mDevice;

        uint32_t mId;
        uint32_t mWidth;
        uint32_t mHeight;
        uint32_t mFormat;
        size_t   mSize;
        uint32_t mUsage;
        uint32_t mMaxProducerBuffers;
        uint32_t mMaxConsumerBuffers;
        uint32_t mTotalBuffers;
        int mFormatRequested;

        /** Debugging information */
        uint32_t mActiveBuffers;
        uint32_t mFrameCount;
        int64_t  mLastTimestamp;

        const camera2_stream_ops *getStreamOps();

        static ANativeWindow* toANW(const camera2_stream_ops_t *w);

        static int dequeue_buffer(const camera2_stream_ops_t *w,
                buffer_handle_t** buffer);

        static int enqueue_buffer(const camera2_stream_ops_t* w,
                int64_t timestamp,
                buffer_handle_t* buffer);

        static int cancel_buffer(const camera2_stream_ops_t* w,
                buffer_handle_t* buffer);

        static int set_crop(const camera2_stream_ops_t* w,
                int left, int top, int right, int bottom);
    }; // class StreamAdapter

    typedef List<sp<StreamAdapter> > StreamList;
    StreamList mStreams;

}; // class Camera2Device

}; // namespace android

#endif
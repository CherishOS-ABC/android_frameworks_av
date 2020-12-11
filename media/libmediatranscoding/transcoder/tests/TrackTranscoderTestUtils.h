/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <media/MediaTrackTranscoder.h>
#include <media/MediaTrackTranscoderCallback.h>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace android {

//
// This file contains test utilities used by more than one track transcoder test.
//

class TrackTranscoderTestUtils {
public:
    static std::shared_ptr<AMediaFormat> getDefaultVideoDestinationFormat(
            AMediaFormat* sourceFormat, bool includeBitrate = true) {
        // Default video destination format setup.
        static constexpr float kFrameRate = 30.0f;
        static constexpr int32_t kBitRate = 2 * 1000 * 1000;

        AMediaFormat* destinationFormat = AMediaFormat_new();
        AMediaFormat_copy(destinationFormat, sourceFormat);
        AMediaFormat_setFloat(destinationFormat, AMEDIAFORMAT_KEY_FRAME_RATE, kFrameRate);
        if (includeBitrate) {
            AMediaFormat_setInt32(destinationFormat, AMEDIAFORMAT_KEY_BIT_RATE, kBitRate);
        }

        return std::shared_ptr<AMediaFormat>(destinationFormat, &AMediaFormat_delete);
    }
};

class TestCallback : public MediaTrackTranscoderCallback {
public:
    TestCallback() = default;
    ~TestCallback() = default;

    // MediaTrackTranscoderCallback
    void onTrackFormatAvailable(const MediaTrackTranscoder* transcoder __unused) {
        std::unique_lock<std::mutex> lock(mMutex);
        mTrackFormatAvailable = true;
        mTrackFormatAvailableCondition.notify_all();
    }

    void onTrackFinished(const MediaTrackTranscoder* transcoder __unused) {
        std::unique_lock<std::mutex> lock(mMutex);
        mTranscodingFinished = true;
        mTranscodingFinishedCondition.notify_all();
    }

    virtual void onTrackStopped(const MediaTrackTranscoder* transcoder __unused) override {
        std::unique_lock<std::mutex> lock(mMutex);
        mTranscodingFinished = true;
        mTranscodingStopped = true;
        mTranscodingFinishedCondition.notify_all();
    }

    void onTrackError(const MediaTrackTranscoder* transcoder __unused, media_status_t status) {
        std::unique_lock<std::mutex> lock(mMutex);
        mTranscodingFinished = true;
        mStatus = status;
        mTranscodingFinishedCondition.notify_all();
    }
    // ~MediaTrackTranscoderCallback

    media_status_t waitUntilFinished() {
        std::unique_lock<std::mutex> lock(mMutex);
        while (!mTranscodingFinished) {
            mTranscodingFinishedCondition.wait(lock);
        }
        return mStatus;
    }

    void waitUntilTrackFormatAvailable() {
        std::unique_lock<std::mutex> lock(mMutex);
        while (!mTrackFormatAvailable) {
            mTrackFormatAvailableCondition.wait(lock);
        }
    }

    bool transcodingWasStopped() const { return mTranscodingFinished && mTranscodingStopped; }
    bool transcodingFinished() const {
        return mTranscodingFinished && !mTranscodingStopped && mStatus == AMEDIA_OK;
    }

private:
    media_status_t mStatus = AMEDIA_OK;
    std::mutex mMutex;
    std::condition_variable mTranscodingFinishedCondition;
    std::condition_variable mTrackFormatAvailableCondition;
    bool mTranscodingFinished = false;
    bool mTranscodingStopped = false;
    bool mTrackFormatAvailable = false;
};

class OneShotSemaphore {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mMutex);
        while (!mSignaled) {
            mCondition.wait(lock);
        }
    }

    void signal() {
        std::unique_lock<std::mutex> lock(mMutex);
        mSignaled = true;
        mCondition.notify_all();
    }

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mSignaled = false;
};

};  // namespace android